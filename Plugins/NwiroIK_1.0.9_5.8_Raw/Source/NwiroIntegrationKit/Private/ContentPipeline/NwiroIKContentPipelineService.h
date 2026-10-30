// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ContentPipeline/NwiroIKContentPipelineTypes.h"

class FNwiroIKContentPipelineService
{
public:
	static FNwiroIKContentPipelineService& Get();
	static void ShutdownSingleton();

	bool StartDownloadAndImport(const FNwiroIKContentRequest& Request, FString& OutError);
	bool StartImportFromCache(const FString& ContentId, const FString& Hash, const FString& RelativeImportPath, FString& OutError);
	bool CancelOperation(const FString& ContentId, const FString& Hash, FString& OutError);
	bool DeleteCachedContent(const FString& Hash, bool bDeleteImportedInProject, FString& OutError);

	FString GetOperationStatusJson(const FString& ContentId, const FString& Hash);
	FString GetContentStateJson(const FString& ContentId, const FString& Hash);
	FString GetCachedContentsJson() const;

	bool IsContentCached(const FString& Hash) const;
	bool IsZipDownloaded(const FString& Hash) const;
	bool IsUnzipComplete(const FString& Hash) const;
	bool IsImportedInCurrentProject(const FString& Hash) const;
	~FNwiroIKContentPipelineService();

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;

	FNwiroIKContentPipelineService();
};
