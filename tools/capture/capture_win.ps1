#
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
#

param(
    [Alias("d")]
    [int]$Duration = 30,

    [Alias("n")]
    [string]$Name = "capture",

    [Alias("i")]
    [string]$Interface = "1",

    [Alias("f")]
    [string]$Filter = "",

    [switch]$Help,

    [Alias("l")]
    [switch]$ListInterfaces
)

function Show-Help {
    Write-Host @"
�÷�: .\capture_win.ps1 [����]

����:
    -d, -Duration         ץ������ʱ�䣨�룬Ĭ�ϣ�30��
    -n, -Name             ����ļ���ǰ׺��Ĭ�ϣ�capture��
    -i, -Interface        ����ӿڱ�ţ�Ĭ�ϣ�1��
    -f, -Filter           BPF��������
    -l, -ListInterfaces   �г���������ӿ�
    -Help                 ��ʾ������Ϣ

ʾ��:
    .\capture_win.ps1 -d 30 -n bilibili
    .\capture_win.ps1 -d 60 -i 2 -f "port 443"
    .\capture_win.ps1 -l
    .\capture_win.ps1 -Help

˵��:
    - �ļ���ֻ�ܰ�����ĸ�����֡��»��ߺ����ַ�
    - ����ӿڱ�ű�������Ч�Ľӿ�ID��ʹ�� -l �鿴��
"@
}

function Test-TsharkPath {
    try {
        $cmd = Get-Command tshark -ErrorAction Stop
        return $cmd.Source
    }
    catch {
        $paths = @(
            "C:\Program Files\Wireshark\tshark.exe",
            "C:\Program Files (x86)\Wireshark\tshark.exe"
        )

        foreach ($path in $paths) {
            if (Test-Path $path) {
                return $path
            }
        }
        return $null
    }
}

function Get-ValidInterfaces {
    param([string]$TsharkPath)

    $output = & $TsharkPath -D 2>&1
    $interfaces = @()

    foreach ($line in $output) {
        if ($line -match '^(\d+)\.') {
            $interfaces += $matches[1]
        }
    }

    return $interfaces
}

function Show-Interfaces {
    param([string]$TsharkPath)

    Write-Host "========== ���õ�����ӿ� =========="
    & $TsharkPath -D
    Write-Host "===================================="
}

# �������
if ($Help) {
    Show-Help
    exit 0
}

# ���tshark
$tsharkPath = Test-TsharkPath
if (-not $tsharkPath) {
    Write-Error "����: δ�ҵ�tshark���ߣ��밲װWireshark"
    exit 1
}

# ����ӿ��б�
if ($ListInterfaces) {
    Show-Interfaces -TsharkPath $tsharkPath
    exit 0
}

# ��֤����ʱ��
if ($Duration -le 0) {
    Write-Error "����: ����ʱ�������������"
    exit 1
}

# ��֤�ļ���
if ($Name -notmatch '^[a-zA-Z0-9_-]+$') {
    Write-Error "����: �ļ���ֻ�ܰ�����ĸ�����֡��»��ߺ����ַ�"
    exit 1
}

# ��֤����ӿ�
$validInterfaces = Get-ValidInterfaces -TsharkPath $tsharkPath
if ($validInterfaces.Count -eq 0) {
    Write-Error "����: �޷���ȡ����ӿ��б�"
    exit 1
}

if ($Interface -notmatch '^\d+$') {
    Write-Error "����: ����ӿڱ���������"
    Write-Host "ʹ�� -l �鿴���õ�����ӿ�"
    exit 1
}

if ($validInterfaces -notcontains $Interface) {
    Write-Error "����: ����ӿ� '$Interface' ������"
    Write-Host "`n���õ�����ӿ�:"
    Show-Interfaces -TsharkPath $tsharkPath
    exit 1
}

# �������
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$filename = "${Name}_${timestamp}.pcap"
$outdir = "C:\flow_capture\$Name"

if (!(Test-Path $outdir)) {
    New-Item -ItemType Directory -Path $outdir -Force | Out-Null
    Write-Host "�Ѵ������Ŀ¼: $outdir"
}

$outputfile = "$outdir\$filename"

# ��ʾ����
Write-Host "========== ץ������ =========="
Write-Host "����·��: $tsharkPath"
Write-Host "����ӿ�: $Interface"
Write-Host "����ʱ��: $Duration ��"
Write-Host "����ļ�: $outputfile"
if ($Filter) {
    Write-Host "��������: $Filter"
}
Write-Host "=============================="

# ִ��ץ��
Write-Host "��ʼץ��..."
try {
    if ($Filter) {
        & $tsharkPath -i $Interface -a "duration:$Duration" -w $outputfile -f $Filter
    }
    else {
        & $tsharkPath -i $Interface -a "duration:$Duration" -w $outputfile
    }

    if ($LASTEXITCODE -eq 0) {
        Write-Host "ץ�����"
    }
    else {
        Write-Error "tsharkִ��ʧ�ܣ��˳�����: $LASTEXITCODE"
        exit 1
    }
}
catch {
    Write-Error "ץ�����̳���: $_"
    exit 1
}

# �����
if (Test-Path $outputfile) {
    $fileInfo = Get-Item $outputfile
    $sizeKB = [math]::Round($fileInfo.Length / 1KB, 2)

    Write-Host "========== ץ����� =========="
    Write-Host "�ļ�λ��: $outputfile"
    Write-Host "�ļ���С: $sizeKB KB"
    Write-Host "=============================="
}
else {
    Write-Error "ץ���ļ�δ����"
    exit 1
}