// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Data structure tools: DataTable, UserDefinedStruct, UserDefinedEnum.
 */
class FNwiroIKDataTools
{
public:
	// Create a DataTable asset
	static FString CreateDataTable(const FString& JsonCommand);

	// Add a row to an existing DataTable
	static FString AddDataTableRow(const FString& JsonCommand);

	// Read all rows from a DataTable
	static FString ReadDataTable(const FString& JsonCommand);

	// Import rows from JSON into a DataTable
	static FString ImportDataTableJson(const FString& JsonCommand);

	// Create a UserDefinedStruct
	static FString CreateStruct(const FString& JsonCommand);

	// Create a UserDefinedEnum
	static FString CreateEnum(const FString& JsonCommand);
};
