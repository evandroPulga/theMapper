<#
.SYNOPSIS
    Executes Python code in the active Blender session via the Blender MCP socket server.

.EXAMPLE
    .\tools\blender_exec.ps1 -Code "import bpy; result = {'objects': [o.name for o in bpy.data.objects]}"
    .\tools\blender_exec.ps1 -Code "import bpy; print('Active scene:', bpy.context.scene.name)"
    .\tools\blender_exec.ps1 -File "path\to\script.py"
#>
param(
    [Parameter(Mandatory=$false, Position=0)]
    [string]$Code,

    [Parameter(Mandatory=$false)]
    [string]$File,

    [string]$HostName = "127.0.0.1",
    [int]$Port = 9876,
    [int]$TimeoutSec = 15
)

if ($File) {
    if (-not (Test-Path $File)) {
        Write-Error "Script file not found: $File"
        exit 1
    }
    $Code = Get-Content -Raw -Encoding UTF8 $File
}

if (-not $Code) {
    Write-Error "No Python code provided. Use -Code '...' or -File 'script.py'"
    exit 1
}

try {
    $client = New-Object System.Net.Sockets.TcpClient
    $connectTask = $client.ConnectAsync($HostName, $Port)
    if (-not $connectTask.Wait($TimeoutSec * 1000)) {
        throw "Connection to Blender on ${HostName}:${Port} timed out. Ensure Blender is running with the MCP add-on enabled."
    }

    $stream = $client.GetStream()
    $stream.ReadTimeout = $TimeoutSec * 1000
    $stream.WriteTimeout = $TimeoutSec * 1000

    $requestObj = @{
        type = "execute"
        code = $Code
        strict_json = $false
    }
    $json = ($requestObj | ConvertTo-Json -Compress) + "`0"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()

    $ms = New-Object System.IO.MemoryStream
    $buffer = New-Object byte[] 4096

    while ($true) {
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -le 0) { break }
        $ms.Write($buffer, 0, $read)
        if ($buffer[0..($read-1)] -contains 0) { break }
    }

    $responseBytes = $ms.ToArray()
    $responseStr = [System.Text.Encoding]::UTF8.GetString($responseBytes).Trim([char]0)
    Write-Output $responseStr

    $client.Close()
} catch {
    Write-Error "Blender MCP error: $($_.Exception.Message)"
    exit 1
}
