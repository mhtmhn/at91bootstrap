[CmdletBinding()]
param(
    [string]$Toolchain = 'C:\Git\zephyr-mchp-workspace\.sdk\zephyr-sdk-1.0.1\gnu\arm-zephyr-eabi',
    [string]$Make = 'C:\Program Files\Microchip\MPLABX\v6.30\gnuBins\GnuWin32\bin\make.exe',
    [string]$GitBin = 'C:\Program Files\Git',
    [string]$SamBa = "$PSScriptRoot\build\tools\sam-ba-3.9.2\sam-ba_v3.9.2\sam-ba.exe",
    [string]$BuildDirectory = 'build/curiosity_zephyr_splash'
)

# Build the non-Pro Curiosity Zephyr-loader configuration.
$ErrorActionPreference = 'Stop'
$previousPath = $env:PATH
$previousVersion = $env:VERSION
Push-Location $PSScriptRoot
try {
    foreach ($required in @($Make, "$Toolchain\bin\arm-zephyr-eabi-gcc.exe", "$GitBin\bin\bash.exe", $SamBa)) {
        if (!(Test-Path -LiteralPath $required)) { throw "Required build tool missing: $required" }
    }
    $env:PATH = "$GitBin\usr\bin;$GitBin\bin;$Toolchain\bin;$previousPath"
    $versionLine = Select-String -Path Makefile -Pattern '^export VERSION := (.+)$'
    $version = $versionLine.Matches[0].Groups[1].Value.Trim()
    $env:VERSION = $version
    $buildDirectory = $BuildDirectory.Replace('\', '/')
    $binaryDirectory = "$buildDirectory/binaries"
    New-Item -ItemType Directory -Force config/at91bootstrap-config, $binaryDirectory | Out-Null

    & python scripts/mpconfig/defconfig.py configs/sama7d65_curiositysd1_zephyr_defconfig
    if ($LASTEXITCODE) { throw "Defconfig generation failed: $LASTEXITCODE" }
    & python scripts/mpconfig/genconfig.py --sync-deps config/at91bootstrap-config --header-path config/at91bootstrap-config/autoconf.h
    if ($LASTEXITCODE) { throw "Autoconf generation failed: $LASTEXITCODE" }
    Copy-Item -LiteralPath .config -Destination "$buildDirectory/build.config" -Force

    $crossPrefix = ($Toolchain.Replace('\', '/') + '/bin/arm-zephyr-eabi-')
    $stem = "sama7d65-sdcardboot-zephyr-$version"
    $raw = "$binaryDirectory/$stem.bin"
    $makeArgs = @('-j4', 'SHELL=bash.exe', 'CONFIG_SHELL=bash.exe',
        "CROSS_COMPILE=$crossPrefix", "BUILDDIR=$buildDirectory", 'SCMINFO=',
        ('BUILD_DATE=' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')),
        'HOSTCC=arm-zephyr-eabi-gcc', 'HOSTAR=arm-zephyr-eabi-ar',
        'HOSTAS=arm-zephyr-eabi-as', 'HOSTCPP=arm-zephyr-eabi-cpp',
        'HOSTLD=arm-zephyr-eabi-ld', 'HOSTNM=arm-zephyr-eabi-nm', 'CheckCrossCompile', $raw)
    & $Make @makeArgs *> "$buildDirectory/build.log"
    if ($LASTEXITCODE) {
        Get-Content "$buildDirectory/build.log" -Tail 50
        throw "Compilation failed: $LASTEXITCODE"
    }

    $nmLines = & "$Toolchain\bin\arm-zephyr-eabi-nm.exe" "$binaryDirectory/$stem.elf"
    if ($LASTEXITCODE) { throw 'Could not read linked symbols' }
    $symbols = @{}
    foreach ($line in $nmLines) {
        if ($line -match '^([0-9a-fA-F]+)\s+\w\s+(_sramsize|_romsize|_ebss|_stext)$') {
            $symbols[$Matches[2]] = [Convert]::ToInt32($Matches[1], 16)
        }
    }
    if (!$symbols.ContainsKey('_sramsize')) { throw 'Missing SRAM accounting symbol' }
    if ($symbols['_sramsize'] -gt 65536) { throw 'Build exceeds the configured 64 KiB SRAM-span limit' }

    Push-Location $binaryDirectory
    try {
        & $SamBa -u "gen_image:sama7d65:$stem.bin:$stem-plaintextimg.bin:0:0"
        if ($LASTEXITCODE) { throw 'SAM-BA image generation failed' }
        Copy-Item -LiteralPath "$stem-plaintextimg.bin" -Destination boot.bin -Force
    } finally { Pop-Location }

    # Validate the vendor-generated header and both hashes without connecting to a board.
    $pti = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot "$binaryDirectory/boot.bin"))
    $rawBytes = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $raw))
    $payloadSize = [BitConverter]::ToUInt32($pti, 4)
    $headerDigest = [Security.Cryptography.SHA256]::HashData([byte[]]$pti[0..15])
    $payloadDigest = [Security.Cryptography.SHA256]::HashData([byte[]]$pti[48..(47 + $payloadSize)])
    if ([Text.Encoding]::ASCII.GetString($pti, 0, 4) -ne 'BSP1' -or ($payloadSize % 16) -ne 0 -or $pti.Length -ne ($payloadSize + 80)) {
        throw 'Invalid generated PTI layout'
    }
    if ([Convert]::ToHexString($headerDigest) -ne [Convert]::ToHexString([byte[]]$pti[16..47]) -or
        [Convert]::ToHexString($payloadDigest) -ne [Convert]::ToHexString([byte[]]$pti[(48 + $payloadSize)..($pti.Length - 1)])) {
        throw 'Invalid generated PTI hash'
    }
    if ([Convert]::ToHexString($rawBytes) -ne [Convert]::ToHexString([byte[]]$pti[48..(47 + $rawBytes.Length)])) {
        throw 'Generated PTI payload differs from raw binary'
    }

    $metrics = [ordered]@{
        defconfig = 'configs/sama7d65_curiositysd1_zephyr_defconfig'
        compiler = "$Toolchain\bin\arm-zephyr-eabi-gcc.exe"
        rom_bytes = $symbols['_romsize']
        raw_binary_bytes = $rawBytes.Length
        sramsize_bytes = $symbols['_sramsize']
        ebss_address = ('0x{0:X8}' -f $symbols['_ebss'])
        headroom_under_64k_bytes = 65536 - $symbols['_sramsize']
        physical_sram_remaining_bytes = 131072 - $symbols['_sramsize']
        remaining_after_8k_stack_reserve_bytes = 131072 - $symbols['_sramsize'] - 8192
        pti_bytes = $pti.Length
        pti_padded_payload_bytes = $payloadSize
        pti_header_sha256_verified = $true
        pti_payload_sha256_verified = $true
        pti_sha256 = (Get-FileHash "$binaryDirectory/boot.bin").Hash
    }
    $metrics | ConvertTo-Json | Set-Content -Encoding utf8 "$buildDirectory/metrics.json"
    $metrics | ConvertTo-Json
    Write-Output "Non-Secure ROM boot image: $binaryDirectory/boot.bin"
} finally {
    $env:PATH = $previousPath
    $env:VERSION = $previousVersion
    Pop-Location
}
