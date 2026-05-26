// Copyright 2023 Big Cat Energising. All Rights Reserved.


#include "ThumbnailExporterRenderer.h"
#include "ThumbnailExporterSettings.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "TextureCompiler.h"
#include "Misc/ScopedSlowTask.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ThumbnailRendering/TextureThumbnailRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "CanvasTypes.h"
#include "ShaderCompiler.h"
#include "ContentStreaming.h"
#include "ThumbnailExporterThumbnailDummy.h"
#include "BlueprintThumbnailExporterRenderer.h"

#if ENGINE_MINOR_VERSION == 0
static void TransitionAndCopyTexture(FRHICommandList& RHICmdList, FRHITexture* Source, FRHITexture* Destination, const FRHICopyTextureInfo& CopyInfo)
{
	FRHITransitionInfo TransitionsBefore[] = {
		FRHITransitionInfo(Source, ERHIAccess::SRVMask, ERHIAccess::CopySrc),
		FRHITransitionInfo(Destination, ERHIAccess::SRVMask, ERHIAccess::CopyDest)
	};

	RHICmdList.Transition(MakeArrayView(TransitionsBefore, UE_ARRAY_COUNT(TransitionsBefore)));

	RHICmdList.CopyTexture(Source, Destination, CopyInfo);

	FRHITransitionInfo TransitionsAfter[] = {
		FRHITransitionInfo(Source, ERHIAccess::CopySrc, ERHIAccess::SRVMask),
		FRHITransitionInfo(Destination, ERHIAccess::CopyDest, ERHIAccess::SRVMask)
	};

	RHICmdList.Transition(MakeArrayView(TransitionsAfter, UE_ARRAY_COUNT(TransitionsAfter)));
}
#endif

FObjectThumbnail* FThumbnailExporterRenderer::GenerateThumbnail(FThumbnailCreationConfig& CreationConfig, UObject* InObject, const FPreCreateThumbnail& CreationDelegate)
{
	// Does the object support thumbnails?
	FThumbnailRenderingInfo* RenderInfo = GUnrealEd ? GUnrealEd->GetThumbnailManager()->GetRenderingInfo(UThumbnailExporterThumbnailDummy::StaticClass()->ClassDefaultObject) : nullptr;
	if (RenderInfo != NULL && RenderInfo->Renderer != nullptr)
	{
		// Set the size of cached thumbnails
		const int32 ImageWidth = CreationConfig.ThumbnailSize;
		const int32 ImageHeight = CreationConfig.ThumbnailSize;

		// For cached thumbnails we want to make sure that textures are fully streamed in so that the thumbnail we're saving won't have artifacts
		// However, this can add 30s - 100s to editor load
		//@todo - come up with a cleaner solution for this, preferably not blocking on texture streaming at all but updating when textures are fully streamed in
		ThumbnailTools::EThumbnailTextureFlushMode::Type TextureFlushMode = ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush;

		// Generate the thumbnail
		FObjectThumbnail NewThumbnail;
		FThumbnailExporterRenderer::RenderThumbnail(
			CreationConfig, InObject, ImageWidth, ImageHeight, TextureFlushMode,
			&NewThumbnail, CreationDelegate);

		UPackage* MyOutermostPackage = InObject->GetOutermost();
		return ThumbnailTools::CacheThumbnail(InObject->GetFullName(), &NewThumbnail, MyOutermostPackage);
	}

	return NULL;
}

struct FThumbnailRenderTargetResource
{
	UTextureRenderTarget2D* RenderTargetTexture;
	FTextureRenderTargetResource* RenderTargetResource;
	FCanvas Canvas;
};

static bool GetAlphaBounds(const TArray<uint8>& ImageData, int32 Width, int32 Height, FIntRect& OutRect)
{
	if (ImageData.Num() < (Width * Height * static_cast<int32>(sizeof(FColor))))
	{
		return false;
	}

	const FColor* Pixels = reinterpret_cast<const FColor*>(ImageData.GetData());

	// 使用 Alpha 阈值过滤低值噪点（Launcher 版引擎背景有渲染伪影）
	const uint8 AlphaThreshold = 10;

	int32 MinX = Width;
	int32 MinY = Height;
	int32 MaxX = -1;
	int32 MaxY = -1;

	for (int32 Y = 0; Y < Height; ++Y)
	{
		const int32 RowOffset = Y * Width;
		for (int32 X = 0; X < Width; ++X)
		{
			if (Pixels[RowOffset + X].A > AlphaThreshold)  // 改为阈值比较
			{
				MinX = FMath::Min(MinX, X);
				MinY = FMath::Min(MinY, Y);
				MaxX = FMath::Max(MaxX, X);
				MaxY = FMath::Max(MaxY, Y);
			}
		}
	}

	if (MaxX < MinX || MaxY < MinY)
	{
		return false;
	}

	OutRect = FIntRect(MinX, MinY, MaxX + 1, MaxY + 1);
	return true;
}

static void ResizeImageBilinear(const TArray<FColor>& Src, int32 SrcWidth, int32 SrcHeight, TArray<FColor>& Dst, int32 DstWidth, int32 DstHeight)
{
	Dst.SetNumUninitialized(DstWidth * DstHeight);
	if (SrcWidth <= 0 || SrcHeight <= 0 || DstWidth <= 0 || DstHeight <= 0)
	{
		return;
	}

	const float ScaleX = (DstWidth > 1) ? (static_cast<float>(SrcWidth - 1) / static_cast<float>(DstWidth - 1)) : 0.0f;
	const float ScaleY = (DstHeight > 1) ? (static_cast<float>(SrcHeight - 1) / static_cast<float>(DstHeight - 1)) : 0.0f;

	for (int32 Y = 0; Y < DstHeight; ++Y)
	{
		const float SrcY = ScaleY * static_cast<float>(Y);
		const int32 Y0 = FMath::Clamp(static_cast<int32>(SrcY), 0, SrcHeight - 1);
		const int32 Y1 = FMath::Clamp(Y0 + 1, 0, SrcHeight - 1);
		const float TY = SrcY - static_cast<float>(Y0);

		for (int32 X = 0; X < DstWidth; ++X)
		{
			const float SrcX = ScaleX * static_cast<float>(X);
			const int32 X0 = FMath::Clamp(static_cast<int32>(SrcX), 0, SrcWidth - 1);
			const int32 X1 = FMath::Clamp(X0 + 1, 0, SrcWidth - 1);
			const float TX = SrcX - static_cast<float>(X0);

			const FColor C00 = Src[Y0 * SrcWidth + X0];
			const FColor C10 = Src[Y0 * SrcWidth + X1];
			const FColor C01 = Src[Y1 * SrcWidth + X0];
			const FColor C11 = Src[Y1 * SrcWidth + X1];

			const float InvTX = 1.0f - TX;
			const float InvTY = 1.0f - TY;

			const float W00 = InvTX * InvTY;
			const float W10 = TX * InvTY;
			const float W01 = InvTX * TY;
			const float W11 = TX * TY;

			const float R = C00.R * W00 + C10.R * W10 + C01.R * W01 + C11.R * W11;
			const float G = C00.G * W00 + C10.G * W10 + C01.G * W01 + C11.G * W11;
			const float B = C00.B * W00 + C10.B * W10 + C01.B * W01 + C11.B * W11;
			const float A = C00.A * W00 + C10.A * W10 + C01.A * W01 + C11.A * W11;

			Dst[Y * DstWidth + X] = FColor(
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(R), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(G), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(B), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(A), 0, 255)));
		}
	}
}

// 整数倍 box 下采样：将每个 N×N 像素块平均，得到平滑的反走样结果。
// 失败时返回 false，调用方应回退到双线性。
static bool ResizeImageBoxIntegerDownsample(const TArray<FColor>& Src, int32 SrcWidth, int32 SrcHeight, TArray<FColor>& Dst, int32 DstWidth, int32 DstHeight)
{
	if (DstWidth <= 0 || DstHeight <= 0 || SrcWidth <= 0 || SrcHeight <= 0)
	{
		return false;
	}
	if ((SrcWidth % DstWidth) != 0 || (SrcHeight % DstHeight) != 0)
	{
		return false;
	}
	const int32 StepX = SrcWidth / DstWidth;
	const int32 StepY = SrcHeight / DstHeight;
	if (StepX <= 1 && StepY <= 1)
	{
		return false;
	}
	Dst.SetNumUninitialized(DstWidth * DstHeight);
	const float InvSampleCount = 1.0f / static_cast<float>(StepX * StepY);
	for (int32 Y = 0; Y < DstHeight; ++Y)
	{
		for (int32 X = 0; X < DstWidth; ++X)
		{
			int32 SumR = 0, SumG = 0, SumB = 0, SumA = 0;
			const int32 BaseX = X * StepX;
			const int32 BaseY = Y * StepY;
			for (int32 Ky = 0; Ky < StepY; ++Ky)
			{
				const int32 SrcY = BaseY + Ky;
				for (int32 Kx = 0; Kx < StepX; ++Kx)
				{
					const FColor& C = Src[SrcY * SrcWidth + (BaseX + Kx)];
					SumR += C.R; SumG += C.G; SumB += C.B; SumA += C.A;
				}
			}
			Dst[Y * DstWidth + X] = FColor(
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SumR * InvSampleCount), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SumG * InvSampleCount), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SumB * InvSampleCount), 0, 255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SumA * InvSampleCount), 0, 255)));
		}
	}
	return true;
}

// 轻微 Unsharp 锐化（3x3）：使用 box blur 估计低通，再做 (1+a)*src - a*blur。
// Amount = 0 → 无效；推荐 0.15~0.5。
static void ApplyMildUnsharp(TArray<uint8>& InOutData, int32 Width, int32 Height, float Amount)
{
	if (Amount <= KINDA_SMALL_NUMBER || Width < 3 || Height < 3)
	{
		return;
	}
	if (InOutData.Num() != Width * Height * static_cast<int32>(sizeof(FColor)))
	{
		return;
	}
	FColor* Src = reinterpret_cast<FColor*>(InOutData.GetData());
	TArray<FColor> Tmp;
	Tmp.SetNumUninitialized(Width * Height);
	FMemory::Memcpy(Tmp.GetData(), Src, Width * Height * sizeof(FColor));

	auto Sample = [&](int32 X, int32 Y) -> const FColor&
	{
		return Tmp[FMath::Clamp(Y, 0, Height - 1) * Width + FMath::Clamp(X, 0, Width - 1)];
	};

	const float OneMinus = 1.0f + Amount;
	const float BlurW = Amount / 9.0f;

	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			float SumR = 0.0f, SumG = 0.0f, SumB = 0.0f;
			for (int32 Dy = -1; Dy <= 1; ++Dy)
			{
				for (int32 Dx = -1; Dx <= 1; ++Dx)
				{
					const FColor& C = Sample(X + Dx, Y + Dy);
					SumR += C.R; SumG += C.G; SumB += C.B;
				}
			}
			const FColor& Center = Tmp[Y * Width + X];
			const float R = Center.R * OneMinus - SumR * BlurW;
			const float G = Center.G * OneMinus - SumG * BlurW;
			const float B = Center.B * OneMinus - SumB * BlurW;
			FColor& Out = Src[Y * Width + X];
			Out.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(R), 0, 255));
			Out.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(G), 0, 255));
			Out.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(B), 0, 255));
			// Alpha 保持不变
		}
	}
}

static void AutoCropAndScaleThumbnail(FObjectThumbnail& Thumbnail, int32 MaxEdgeSize, bool bCropX, bool bCropY)
{
	if (MaxEdgeSize <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - MaxEdgeSize <= 0, returning early"));
		return;
	}

	const int32 Width = Thumbnail.GetImageWidth();
	const int32 Height = Thumbnail.GetImageHeight();
	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Invalid dimensions: %d x %d, returning early"), Width, Height);
		return;
	}

	TArray<uint8>& ImageData = Thumbnail.AccessImageData();
	UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - ImageData size: %d bytes, expected: %d bytes"), ImageData.Num(), Width * Height * 4);

	// 检查 Alpha 通道的分布
	if (ImageData.Num() >= Width * Height * 4)
	{
		const FColor* Pixels = reinterpret_cast<const FColor*>(ImageData.GetData());
		int32 ZeroAlphaCount = 0;
		int32 FullAlphaCount = 0;
		int32 PartialAlphaCount = 0;
		for (int32 i = 0; i < Width * Height; ++i)
		{
			if (Pixels[i].A == 0) ++ZeroAlphaCount;
			else if (Pixels[i].A == 255) ++FullAlphaCount;
			else ++PartialAlphaCount;
		}
		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Alpha distribution: Zero=%d, Full=%d, Partial=%d (Total=%d)"),
			ZeroAlphaCount, FullAlphaCount, PartialAlphaCount, Width * Height);
	}

	// 检查四个角落的 Alpha 值
	if (ImageData.Num() >= Width * Height * 4)
	{
		const FColor* Pixels = reinterpret_cast<const FColor*>(ImageData.GetData());
		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Corner Alpha values: TopLeft=%d, TopRight=%d, BottomLeft=%d, BottomRight=%d"),
			Pixels[0].A,
			Pixels[Width - 1].A,
			Pixels[(Height - 1) * Width].A,
			Pixels[(Height - 1) * Width + (Width - 1)].A);

		// 检查边缘第一行和最后一行是否有非零 Alpha
		int32 TopRowNonZero = 0, BottomRowNonZero = 0, LeftColNonZero = 0, RightColNonZero = 0;
		for (int32 X = 0; X < Width; ++X)
		{
			if (Pixels[X].A > 0) ++TopRowNonZero;
			if (Pixels[(Height - 1) * Width + X].A > 0) ++BottomRowNonZero;
		}
		for (int32 Y = 0; Y < Height; ++Y)
		{
			if (Pixels[Y * Width].A > 0) ++LeftColNonZero;
			if (Pixels[Y * Width + (Width - 1)].A > 0) ++RightColNonZero;
		}
		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Edge non-zero alpha: Top=%d, Bottom=%d, Left=%d, Right=%d"),
			TopRowNonZero, BottomRowNonZero, LeftColNonZero, RightColNonZero);
	}

	FIntRect Bounds;
	if (!GetAlphaBounds(ImageData, Width, Height, Bounds))
	{
		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - GetAlphaBounds returned false, no valid alpha bounds found"));
		return;
	}

	// 根据裁剪参数调整边界
	// 单轴裁剪模式（输出方形）时，两个轴都紧贴内容裁剪以最大化利用画面
	// 仅在两轴都未选中时才保留完整尺寸（理论上不会进入此函数）
	const bool bSquareOutput = (bCropX != bCropY); // 单轴裁剪 → 方形输出
	if (!bCropX && !bSquareOutput)
	{
		// 不裁剪 X 方向，保持完整宽度
		Bounds.Min.X = 0;
		Bounds.Max.X = Width;
	}
	if (!bCropY && !bSquareOutput)
	{
		// 不裁剪 Y 方向，保持完整高度
		Bounds.Min.Y = 0;
		Bounds.Max.Y = Height;
	}

	UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Bounds after adjustment (CropX=%d, CropY=%d): Min(%d,%d) Max(%d,%d), CropSize: %d x %d"),
		bCropX, bCropY, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y, Bounds.Width(), Bounds.Height());

	// 如果边界等于整个图像，说明边缘有噪点，尝试使用阈值过滤
	if (Bounds.Min.X == 0 && Bounds.Min.Y == 0 && Bounds.Max.X == Width && Bounds.Max.Y == Height)
	{
		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Bounds equal to full image, edge pixels may have low alpha noise"));
	}

	const int32 CropWidth = Bounds.Width();
	const int32 CropHeight = Bounds.Height();
	if (CropWidth <= 0 || CropHeight <= 0)
	{
		return;
	}

	TArray<FColor> Cropped;
	Cropped.SetNumUninitialized(CropWidth * CropHeight);
	const FColor* Src = reinterpret_cast<const FColor*>(ImageData.GetData());
	for (int32 Y = 0; Y < CropHeight; ++Y)
	{
		const int32 SrcOffset = (Bounds.Min.Y + Y) * Width + Bounds.Min.X;
		FMemory::Memcpy(&Cropped[Y * CropWidth], &Src[SrcOffset], sizeof(FColor) * CropWidth);
	}

	// 判断裁剪模式
	const bool bBothAxesCrop = bCropX && bCropY;
	const bool bOnlyXCrop = bCropX && !bCropY;
	const bool bOnlyYCrop = !bCropX && bCropY;

	int32 ScaledWidth, ScaledHeight;
	int32 FinalWidth, FinalHeight;

	if (bBothAxesCrop)
	{
		// 两轴都裁剪：保持宽高比，最长边为 MaxEdgeSize，输出可能是非方形
		if (CropWidth >= CropHeight)
		{
			ScaledWidth = MaxEdgeSize;
			ScaledHeight = FMath::Max(1, FMath::RoundToInt(static_cast<float>(MaxEdgeSize) * static_cast<float>(CropHeight) / static_cast<float>(CropWidth)));
		}
		else
		{
			ScaledHeight = MaxEdgeSize;
			ScaledWidth = FMath::Max(1, FMath::RoundToInt(static_cast<float>(MaxEdgeSize) * static_cast<float>(CropWidth) / static_cast<float>(CropHeight)));
		}
		FinalWidth = ScaledWidth;
		FinalHeight = ScaledHeight;
		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Both axes crop: aspect ratio preserved, output size: %d x %d"), FinalWidth, FinalHeight);
	}
	else if (bOnlyXCrop)
	{
		// 只裁剪X轴：裁剪后内容等比缩放适配方形画布（最长边填满，短边居中，不裁切），强制输出方形
		FinalWidth = MaxEdgeSize;
		FinalHeight = MaxEdgeSize;

		// 等比缩放：最长边填满画布，短边居中，不裁切
		if (CropWidth >= CropHeight)
		{
			ScaledWidth = MaxEdgeSize;
			ScaledHeight = FMath::Max(1, FMath::RoundToInt(static_cast<float>(MaxEdgeSize) * static_cast<float>(CropHeight) / static_cast<float>(CropWidth)));
		}
		else
		{
			ScaledHeight = MaxEdgeSize;
			ScaledWidth = FMath::Max(1, FMath::RoundToInt(static_cast<float>(MaxEdgeSize) * static_cast<float>(CropWidth) / static_cast<float>(CropHeight)));
		}

		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Only X crop: contain fit, scaled %d x %d, output square %d x %d"),
			ScaledWidth, ScaledHeight, FinalWidth, FinalHeight);
	}
	else if (bOnlyYCrop)
	{
		// 只裁剪Y轴：裁剪后内容等比缩放适配方形画布（最长边填满，短边居中，不裁切），强制输出方形
		FinalWidth = MaxEdgeSize;
		FinalHeight = MaxEdgeSize;

		// 等比缩放：最长边填满画布，短边居中，不裁切
		if (CropHeight >= CropWidth)
		{
			ScaledHeight = MaxEdgeSize;
			ScaledWidth = FMath::Max(1, FMath::RoundToInt(static_cast<float>(MaxEdgeSize) * static_cast<float>(CropWidth) / static_cast<float>(CropHeight)));
		}
		else
		{
			ScaledWidth = MaxEdgeSize;
			ScaledHeight = FMath::Max(1, FMath::RoundToInt(static_cast<float>(MaxEdgeSize) * static_cast<float>(CropHeight) / static_cast<float>(CropWidth)));
		}

		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Only Y crop: contain fit, scaled %d x %d, output square %d x %d"),
			ScaledWidth, ScaledHeight, FinalWidth, FinalHeight);
	}
	else
	{
		// 都不裁剪（理论上不应该进入这个函数）
		ScaledWidth = CropWidth;
		ScaledHeight = CropHeight;
		FinalWidth = CropWidth;
		FinalHeight = CropHeight;
	}

	// 第一步：缩放裁剪后的内容
	TArray<FColor> Resized;
	if (ScaledWidth != CropWidth || ScaledHeight != CropHeight)
	{
		ResizeImageBilinear(Cropped, CropWidth, CropHeight, Resized, ScaledWidth, ScaledHeight);
	}
	else
	{
		Resized = MoveTemp(Cropped);
	}

	// 第二步：单轴裁剪时，将缩放后的内容放置到方形画布上
	if (bOnlyXCrop || bOnlyYCrop)
	{
		TArray<FColor> Canvas;
		Canvas.SetNumZeroed(FinalWidth * FinalHeight);

		// 计算偏移（居中或裁剪）
		// 如果缩放后尺寸小于画布，居中放置（正偏移）
		// 如果缩放后尺寸大于画布，裁剪中间部分（负偏移表示从源图像的哪里开始取）
		const int32 OffsetX = (FinalWidth - ScaledWidth) / 2;
		const int32 OffsetY = (FinalHeight - ScaledHeight) / 2;

		// 源图像的起始位置（如果需要裁剪）
		const int32 SrcStartX = (OffsetX < 0) ? -OffsetX : 0;
		const int32 SrcStartY = (OffsetY < 0) ? -OffsetY : 0;

		// 目标画布的起始位置（如果需要居中）
		const int32 DstStartX = (OffsetX > 0) ? OffsetX : 0;
		const int32 DstStartY = (OffsetY > 0) ? OffsetY : 0;

		// 实际复制的宽度和高度（取两者的最小值）
		const int32 CopyWidth = FMath::Min(ScaledWidth - SrcStartX, FinalWidth - DstStartX);
		const int32 CopyHeight = FMath::Min(ScaledHeight - SrcStartY, FinalHeight - DstStartY);

		UE_LOG(LogTemp, Warning, TEXT("AutoCropAndScaleThumbnail - Scaled: %d x %d, Canvas: %d x %d, Src offset: (%d, %d), Dst offset: (%d, %d), Copy: %d x %d"),
			ScaledWidth, ScaledHeight, FinalWidth, FinalHeight, SrcStartX, SrcStartY, DstStartX, DstStartY, CopyWidth, CopyHeight);

		// 复制内容到画布
		for (int32 Y = 0; Y < CopyHeight; ++Y)
		{
			for (int32 X = 0; X < CopyWidth; ++X)
			{
				const int32 SrcIndex = (SrcStartY + Y) * ScaledWidth + (SrcStartX + X);
				const int32 DstIndex = (DstStartY + Y) * FinalWidth + (DstStartX + X);

				if (SrcIndex >= 0 && SrcIndex < Resized.Num() && DstIndex >= 0 && DstIndex < Canvas.Num())
				{
					Canvas[DstIndex] = Resized[SrcIndex];
				}
			}
		}

		ImageData.SetNumUninitialized(FinalWidth * FinalHeight * static_cast<int32>(sizeof(FColor)));
		FMemory::Memcpy(ImageData.GetData(), Canvas.GetData(), ImageData.Num());
	}
	else
	{
		ImageData.SetNumUninitialized(FinalWidth * FinalHeight * static_cast<int32>(sizeof(FColor)));
		FMemory::Memcpy(ImageData.GetData(), Resized.GetData(), ImageData.Num());
	}

	Thumbnail.SetImageSize(FinalWidth, FinalHeight);
}

static FColor ToByteColor(const FLinearColor& Color)
{
	return FColor(
		static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Color.R * 255.0f), 0, 255)),
		static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Color.G * 255.0f), 0, 255)),
		static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Color.B * 255.0f), 0, 255)),
		static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Color.A * 255.0f), 0, 255)));
}

static FLinearColor PickAlternateBackground(const FLinearColor& BaseColor)
{
	const float Luma = BaseColor.R * 0.2126f + BaseColor.G * 0.7152f + BaseColor.B * 0.0722f;
	const FLinearColor AltColor = (Luma > 0.5f) ? FLinearColor::Black : FLinearColor::White;
	if (BaseColor.Equals(AltColor, 0.001f))
	{
		return (AltColor == FLinearColor::Black) ? FLinearColor::White : FLinearColor::Black;
	}

	return AltColor;
}

static bool HasFractionalAlpha(const TArray<uint8>& AlphaData, bool bInvertAlpha)
{
	const int32 PixelCount = AlphaData.Num() / static_cast<int32>(sizeof(FColor));
	if (PixelCount <= 0)
	{
		return false;
	}

	const FColor* AlphaPixels = reinterpret_cast<const FColor*>(AlphaData.GetData());
	for (int32 Index = 0; Index < PixelCount; ++Index)
	{
		const uint8 A = bInvertAlpha ? static_cast<uint8>(255 - AlphaPixels[Index].A) : AlphaPixels[Index].A;
		if (A > 0 && A < 255)
		{
			return true;
		}
	}

	return false;
}

static FThumbnailRenderTargetResource CreateThumbnailRenderTarget(uint32 InImageWidth, uint32 InImageHeight, FLinearColor ClearColor, bool bUseHDR = false)
{
	const uint32 MinRenderTargetSize = FMath::Max(InImageWidth, InImageHeight);
	UTextureRenderTarget2D* RenderTargetTexture = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	check(RenderTargetTexture != NULL);

	RenderTargetTexture->TargetGamma = GEngine->DisplayGamma;
	// 使用HDR格式以获得更好的反射和玻璃效果
	RenderTargetTexture->RenderTargetFormat = bUseHDR ? RTF_RGBA16f : RTF_RGBA8;
	RenderTargetTexture->ClearColor = ClearColor;
	RenderTargetTexture->InitAutoFormat(InImageWidth, InImageHeight);
	RenderTargetTexture->UpdateResourceImmediate(true);

	// Make sure the input dimensions are OK.  The requested dimensions must be less than or equal to
	// our scratch render target size.
	check(InImageWidth <= RenderTargetTexture->GetSurfaceWidth());
	check(InImageHeight <= RenderTargetTexture->GetSurfaceHeight());

	FTextureRenderTargetResource* RenderTargetResource = RenderTargetTexture->GameThread_GetRenderTargetResource();
	check(RenderTargetResource != NULL);

	// Create a canvas for the render target and clear it
	FCanvas Canvas(RenderTargetResource, NULL, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
	Canvas.Clear(ClearColor);

	return FThumbnailRenderTargetResource{ RenderTargetTexture, RenderTargetResource, MoveTemp(Canvas)};
}

void FThumbnailExporterRenderer::RenderThumbnail(FThumbnailCreationConfig& CreationConfig, UObject* InObject, 
	const uint32 InImageWidth, const uint32 InImageHeight, ThumbnailTools::EThumbnailTextureFlushMode::Type InFlushMode, FObjectThumbnail* OutThumbnail, const FPreCreateThumbnail& CreationDelegate)
{
	if (!FApp::CanEverRender())
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FThumbnailExporterRenderer::RenderThumbnail);

	// Renderer must be initialized before generating thumbnails
	check(GIsRHIInitialized);

	// 超采样：以 EffWidth * EffHeight 的高分辨率渲染并完成所有 Alpha 推导，
	// 最终在写入 AutoCrop 之前再 box 下采样到目标分辨率以获得反走样效果。
	const uint32 SuperScale = static_cast<uint32>(FMath::Clamp(CreationConfig.SuperSampleScale, 1, 8));
	const uint32 EffWidth = InImageWidth * SuperScale;
	const uint32 EffHeight = InImageHeight * SuperScale;

	// Store dimensions
	if (OutThumbnail)
	{
		OutThumbnail->SetImageSize(EffWidth, EffHeight);
	}

	FThumbnailRenderTargetResource LDRThumbnail = CreateThumbnailRenderTarget(EffWidth, EffHeight, CreationConfig.GetAdjustedBackgroundColor());
	FThumbnailRenderTargetResource AlphaThumbnail = CreateThumbnailRenderTarget(EffWidth, EffHeight, CreationConfig.GetAdjustedBackgroundColor());

	// Get the rendering info for this object
	FThumbnailRenderingInfo* RenderInfo = GUnrealEd ? GUnrealEd->GetThumbnailManager()->GetRenderingInfo(UThumbnailExporterThumbnailDummy::StaticClass()->ClassDefaultObject) : nullptr;
	UBlueprintThumbnailExporterRenderer* OurThumbnailRenderer = nullptr;

	// Wait for all textures to be streamed in before we render the thumbnail
	// @todo CB: This helps but doesn't result in 100%-streamed-in resources every time! :(
	if (InFlushMode == ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush)
	{
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->ProcessAsyncResults(false, true);
		}

		if (UTexture* Texture = Cast<UTexture>(InObject))
		{
			FTextureCompilingManager::Get().FinishCompilation({ Texture });
		}

		FlushAsyncLoading();

		IStreamingManager::Get().StreamAllResources(100.0f);
	}

	if (RenderInfo != NULL && RenderInfo->Renderer != NULL)
	{
		// Make sure we suppress any message dialogs that might result from constructing
		// or initializing any of the renderable objects.
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);

		// Draw the thumbnail
		const bool bAdditionalViewFamily = false;

		OurThumbnailRenderer = Cast<UBlueprintThumbnailExporterRenderer>(RenderInfo->Renderer);
		check(OurThumbnailRenderer != nullptr);

		OurThumbnailRenderer->ResetThumbnailScenes();

		{
			// Draw the LDR/final color thumbnail
			FThumbnailCreationConfig LdrConfig = CreationConfig;
			LdrConfig.ThumbnailCaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
			LdrConfig.ThumbnailCompositeMode = ESceneCaptureCompositeMode::SCCM_Overwrite;
			FThumbnailCreationParams CreationParams(LdrConfig);
			CreationParams.Object = InObject;
			CreationParams.Width = EffWidth;
			CreationParams.Height = EffHeight;
			CreationParams.bIsAlpha = false;
			CreationParams.RenderTarget = LDRThumbnail.RenderTargetResource;
			CreationParams.Canvas = &LDRThumbnail.Canvas;
			CreationParams.bAdditionalViewFamily = bAdditionalViewFamily;
			CreationParams.CreationDelegate = CreationDelegate;

			OurThumbnailRenderer->DrawThumbnailWithConfig(CreationParams);
		}

		{
			// Draw the alpha
			FThumbnailCreationConfig AlphaConfig = CreationConfig;
			FThumbnailCreationParams CreationParams(AlphaConfig);
			CreationParams.Object = InObject;
			CreationParams.Width = EffWidth;
			CreationParams.Height = EffHeight;
			CreationParams.bIsAlpha = true;
			CreationParams.RenderTarget = AlphaThumbnail.RenderTargetResource;
			CreationParams.Canvas = &AlphaThumbnail.Canvas;
			CreationParams.bAdditionalViewFamily = bAdditionalViewFamily;
			CreationParams.CreationDelegate = CreationDelegate;

			OurThumbnailRenderer->DrawThumbnailWithConfig(CreationParams);
		}
	}

	// Tell the rendering thread to draw any remaining batched elements
	LDRThumbnail.Canvas.Flush_GameThread();
	AlphaThumbnail.Canvas.Flush_GameThread();

	ENQUEUE_RENDER_COMMAND(UpdateThumbnailRTCommand)(
		[LDRRenderTargetResource = LDRThumbnail.RenderTargetResource, AlphaRenderTargetResource = AlphaThumbnail.RenderTargetResource](FRHICommandListImmediate& RHICmdList)
		{
			TransitionAndCopyTexture(RHICmdList, LDRRenderTargetResource->GetRenderTargetTexture(), LDRRenderTargetResource->TextureRHI, {});
			TransitionAndCopyTexture(RHICmdList, AlphaRenderTargetResource->GetRenderTargetTexture(), AlphaRenderTargetResource->TextureRHI, {});
		}
	);

	FlushRenderingCommands();

	if (OutThumbnail)
	{
		const FIntRect InSrcRect(0, 0, OutThumbnail->GetImageWidth(), OutThumbnail->GetImageHeight());

		TArray<uint8>& OutData = OutThumbnail->AccessImageData();

		OutData.Empty();
		OutData.AddUninitialized(OutThumbnail->GetImageWidth() * OutThumbnail->GetImageHeight() * sizeof(FColor));

		// Copy the contents of the LDR color to the thumbnail
		// NOTE: OutData must be a preallocated buffer!
		LDRThumbnail.RenderTargetResource->ReadPixelsPtr((FColor*)OutData.GetData(), FReadSurfaceDataFlags(), InSrcRect);

		TArray<uint8> AlphaData;
		AlphaData.AddUninitialized(OutThumbnail->GetImageWidth() * OutThumbnail->GetImageHeight() * sizeof(FColor));

		AlphaThumbnail.RenderTargetResource->ReadPixelsPtr((FColor*)AlphaData.GetData(), FReadSurfaceDataFlags(), InSrcRect);

		const bool bInvertAlpha = CreationConfig.InvertBackgroundAlpha();
		const bool bHasFractionalAlpha = HasFractionalAlpha(AlphaData, bInvertAlpha);
		const bool bTransparentBackground = CreationConfig.ThumbnailBackground.A <= KINDA_SMALL_NUMBER;
		bool bDerivedAlphaFromColor = false;

		if (!bHasFractionalAlpha && bTransparentBackground && OurThumbnailRenderer != nullptr)
		{
			const FLinearColor BaseBackground = CreationConfig.ThumbnailBackground;
			const FLinearColor AltBackground = PickAlternateBackground(BaseBackground);

			FThumbnailRenderTargetResource AltLDRThumbnail = CreateThumbnailRenderTarget(EffWidth, EffHeight, AltBackground);

			{
				FThumbnailCreationConfig AltConfig = CreationConfig;
				AltConfig.ThumbnailBackground = AltBackground;
				AltConfig.ThumbnailCaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
				AltConfig.ThumbnailCompositeMode = ESceneCaptureCompositeMode::SCCM_Overwrite;
				FThumbnailCreationParams CreationParams(AltConfig);
				CreationParams.Object = InObject;
				CreationParams.Width = EffWidth;
				CreationParams.Height = EffHeight;
				CreationParams.bIsAlpha = false;
				CreationParams.RenderTarget = AltLDRThumbnail.RenderTargetResource;
				CreationParams.Canvas = &AltLDRThumbnail.Canvas;
				CreationParams.bAdditionalViewFamily = false;
				CreationParams.CreationDelegate = CreationDelegate;

				OurThumbnailRenderer->DrawThumbnailWithConfig(CreationParams);
			}

			AltLDRThumbnail.Canvas.Flush_GameThread();
			ENQUEUE_RENDER_COMMAND(UpdateAltThumbnailRTCommand)(
				[AltRenderTargetResource = AltLDRThumbnail.RenderTargetResource](FRHICommandListImmediate& RHICmdList)
				{
					TransitionAndCopyTexture(RHICmdList, AltRenderTargetResource->GetRenderTargetTexture(), AltRenderTargetResource->TextureRHI, {});
				}
			);
			FlushRenderingCommands();

			TArray<uint8> AltColorData;
			AltColorData.AddUninitialized(OutThumbnail->GetImageWidth() * OutThumbnail->GetImageHeight() * sizeof(FColor));
			AltLDRThumbnail.RenderTargetResource->ReadPixelsPtr((FColor*)AltColorData.GetData(), FReadSurfaceDataFlags(), InSrcRect);

			const int32 PixelCount = OutData.Num() / static_cast<int32>(sizeof(FColor));
			const int32 Width = OutThumbnail->GetImageWidth();
			const int32 Height = OutThumbnail->GetImageHeight();
			FColor* ColorPixels = reinterpret_cast<FColor*>(OutData.GetData());
			const FColor* AltPixels = reinterpret_cast<const FColor*>(AltColorData.GetData());

			FColor BaseBgColor = ToByteColor(BaseBackground);
			FColor AltBgColor = ToByteColor(AltBackground);
			if (Width > 0 && Height > 0)
			{
				const int32 CornerIndices[4] = {
					0,
					Width - 1,
					(Height - 1) * Width,
					(Height - 1) * Width + (Width - 1)
				};

				int32 BestIndex = CornerIndices[0];
				int32 BestDiff = -1;
				for (int32 Corner = 0; Corner < UE_ARRAY_COUNT(CornerIndices); ++Corner)
				{
					const int32 Index = CornerIndices[Corner];
					if (Index < 0 || Index >= PixelCount)
					{
						continue;
					}

					const FColor C1 = ColorPixels[Index];
					const FColor C2 = AltPixels[Index];
					const int32 Diff = FMath::Abs(static_cast<int32>(C1.R) - static_cast<int32>(C2.R))
						+ FMath::Abs(static_cast<int32>(C1.G) - static_cast<int32>(C2.G))
						+ FMath::Abs(static_cast<int32>(C1.B) - static_cast<int32>(C2.B));
					if (Diff > BestDiff)
					{
						BestDiff = Diff;
						BestIndex = Index;
					}
				}

				if (BestIndex >= 0 && BestIndex < PixelCount)
				{
					BaseBgColor = ColorPixels[BestIndex];
					AltBgColor = AltPixels[BestIndex];
				}
			}
			for (int32 Index = 0; Index < PixelCount; ++Index)
			{
				const FColor C1 = ColorPixels[Index];
				const FColor C2 = AltPixels[Index];

				float AlphaSum = 0.0f;
				int32 AlphaCount = 0;

				const float DenomR = static_cast<float>(BaseBgColor.R) - static_cast<float>(AltBgColor.R);
				if (!FMath::IsNearlyZero(DenomR))
				{
					AlphaSum += 1.0f - (static_cast<float>(C1.R) - static_cast<float>(C2.R)) / DenomR;
					++AlphaCount;
				}
				const float DenomG = static_cast<float>(BaseBgColor.G) - static_cast<float>(AltBgColor.G);
				if (!FMath::IsNearlyZero(DenomG))
				{
					AlphaSum += 1.0f - (static_cast<float>(C1.G) - static_cast<float>(C2.G)) / DenomG;
					++AlphaCount;
				}
				const float DenomB = static_cast<float>(BaseBgColor.B) - static_cast<float>(AltBgColor.B);
				if (!FMath::IsNearlyZero(DenomB))
				{
					AlphaSum += 1.0f - (static_cast<float>(C1.B) - static_cast<float>(C2.B)) / DenomB;
					++AlphaCount;
				}

				float Alpha = (AlphaCount > 0) ? (AlphaSum / static_cast<float>(AlphaCount)) : 1.0f;
				Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
				const uint8 AlphaByte = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Alpha * 255.0f), 0, 255));

				if (AlphaByte == 0)
				{
					ColorPixels[Index] = FColor(0, 0, 0, 0);
					continue;
				}

				const float OneMinusAlpha = 1.0f - Alpha;
				const float InvAlpha = 1.0f / FMath::Max(Alpha, 1e-3f);

				const float R = (static_cast<float>(C1.R) - static_cast<float>(BaseBgColor.R) * OneMinusAlpha) * InvAlpha;
				const float G = (static_cast<float>(C1.G) - static_cast<float>(BaseBgColor.G) * OneMinusAlpha) * InvAlpha;
				const float B = (static_cast<float>(C1.B) - static_cast<float>(BaseBgColor.B) * OneMinusAlpha) * InvAlpha;

				ColorPixels[Index].R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(R), 0, 255));
				ColorPixels[Index].G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(G), 0, 255));
				ColorPixels[Index].B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(B), 0, 255));
				ColorPixels[Index].A = AlphaByte;
			}

			bDerivedAlphaFromColor = true;
		}

		if (!bDerivedAlphaFromColor)
		{
			FColor* Color = (FColor*)OutData.GetData();
			FColor* Alpha = (FColor*)AlphaData.GetData();
			if (bInvertAlpha)
			{
				for (; Color < (FColor*)(OutData.GetData() + OutData.Num()); ++Color, ++Alpha)
				{
					Color->A = 255 - Alpha->A;
				}
			}
			else
			{
				for (; Color < (FColor*)(OutData.GetData() + OutData.Num()); ++Color, ++Alpha)
				{
					Color->A = Alpha->A;
				}
			}

			if (CreationConfig.bUnpremultiplyAlpha)
			{
				for (FColor* Pixel = (FColor*)OutData.GetData(); Pixel < (FColor*)(OutData.GetData() + OutData.Num()); ++Pixel)
				{
					const uint8 A = Pixel->A;
					if (A > 0 && A < 255)
					{
						Pixel->R = static_cast<uint8>(FMath::Clamp((Pixel->R * 255 + (A / 2)) / A, 0, 255));
						Pixel->G = static_cast<uint8>(FMath::Clamp((Pixel->G * 255 + (A / 2)) / A, 0, 255));
						Pixel->B = static_cast<uint8>(FMath::Clamp((Pixel->B * 255 + (A / 2)) / A, 0, 255));
					}
				}
			}
		}

		// 超采样下采样：把高分辨率工作缓冲下采样到目标尺寸（box 整数下采样优先，否则双线性）。
		if (SuperScale > 1)
		{
			TArray<FColor> SrcPixels;
			SrcPixels.SetNumUninitialized(EffWidth * EffHeight);
			FMemory::Memcpy(SrcPixels.GetData(), OutData.GetData(), EffWidth * EffHeight * sizeof(FColor));

			TArray<FColor> DstPixels;
			if (!ResizeImageBoxIntegerDownsample(SrcPixels, EffWidth, EffHeight, DstPixels, InImageWidth, InImageHeight))
			{
				ResizeImageBilinear(SrcPixels, EffWidth, EffHeight, DstPixels, InImageWidth, InImageHeight);
			}

			OutThumbnail->SetImageSize(InImageWidth, InImageHeight);
			TArray<uint8>& Final = OutThumbnail->AccessImageData();
			Final.Empty();
			Final.AddUninitialized(InImageWidth * InImageHeight * sizeof(FColor));
			FMemory::Memcpy(Final.GetData(), DstPixels.GetData(), InImageWidth * InImageHeight * sizeof(FColor));
		}

		// 可选的轻锐化（在目标分辨率上做）
		if (CreationConfig.bEnableMildSharpen && CreationConfig.SharpenAmount > KINDA_SMALL_NUMBER)
		{
			TArray<uint8>& Final = OutThumbnail->AccessImageData();
			ApplyMildUnsharp(Final, OutThumbnail->GetImageWidth(), OutThumbnail->GetImageHeight(), CreationConfig.SharpenAmount);
		}

		if (CreationConfig.bAutoThumbnailSizeX || CreationConfig.bAutoThumbnailSizeY)
		{
			UE_LOG(LogTemp, Warning, TEXT("RenderThumbnail - Auto crop enabled (X=%d, Y=%d), calling AutoCropAndScaleThumbnail with size: %d"),
				CreationConfig.bAutoThumbnailSizeX, CreationConfig.bAutoThumbnailSizeY, CreationConfig.ThumbnailSize);
			AutoCropAndScaleThumbnail(*OutThumbnail, CreationConfig.ThumbnailSize, CreationConfig.bAutoThumbnailSizeX, CreationConfig.bAutoThumbnailSizeY);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("RenderThumbnail - Auto crop disabled"));
		}
	}
}
