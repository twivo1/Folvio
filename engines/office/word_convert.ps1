<#
  Folvio - Microsoft Word conversion helper (optional engine).

  Invoked by Folvio with an argv array (never through string concatenation):
    powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File word_convert.ps1
                   -InputPath <file> -OutputPath <file> -Format <pdf|docx|rtf|txt|html|odt>

  Safety: macros are force-disabled (AutomationSecurity = 3), the source is
  opened read-only, no password is supplied (protected files fail instead of
  prompting), alerts are suppressed and Word is always closed.
#>
param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][ValidateSet('pdf', 'docx', 'rtf', 'txt', 'html', 'odt')][string]$Format
)

$ErrorActionPreference = 'Stop'
$code = 0
$word = $null
$doc = $null
try {
    if (-not (Test-Path -LiteralPath $InputPath -PathType Leaf)) { throw "Input file not found." }
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0             # wdAlertsNone
    $word.AutomationSecurity = 3        # msoAutomationSecurityForceDisable
    $missing = [Type]::Missing
    # Documents.Open(FileName, ConfirmConversions, ReadOnly, AddToRecentFiles,
    #                PasswordDocument, PasswordTemplate, Revert, WritePasswordDocument,
    #                WritePasswordTemplate, Format, Encoding, Visible, OpenAndRepair,
    #                DocumentDirection, NoEncodingDialog)
    $doc = $word.Documents.Open($InputPath, $false, $true, $false, '', '', $false, '', '', 0, $missing, $false, $false, 0, $true)
    $fmt = switch ($Format) {
        'pdf' { 17 }   # wdFormatPDF
        'docx' { 16 }  # wdFormatDocumentDefault
        'rtf' { 6 }    # wdFormatRTF
        'txt' { 7 }    # wdFormatUnicodeText
        'html' { 10 }  # wdFormatFilteredHTML
        'odt' { 23 }   # wdFormatOpenDocumentText
    }
    $doc.SaveAs2([ref]$OutputPath, [ref]$fmt)
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    $code = 2
}
finally {
    if ($doc) { try { $doc.Close([ref]0) | Out-Null } catch {} }
    if ($word) {
        try { $word.Quit([ref]0) | Out-Null } catch {}
        [System.Runtime.InteropServices.Marshal]::ReleaseComObject($word) | Out-Null
    }
    [GC]::Collect()
}
exit $code
