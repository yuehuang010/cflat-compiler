<#
.SYNOPSIS
    Inventory the Windows application-manifest schema from Microsoft Learn.

.DESCRIPTION
    Parses the Elements section of the Microsoft Learn application-manifests
    reference page, including element attributes and documented values. The
    result is printed as a compact summary unless -OutJson is supplied.

    Source: https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests
#>
[CmdletBinding()]
param(
    [string] $FromFile,
    [string] $OutJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$sourceUrl = 'https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests'

function Convert-HtmlText {
    param([string] $Html)

    $text = [regex]::Replace($Html, '(?is)<br\s*/?>', ' ')
    $text = [regex]::Replace($text, '(?is)<[^>]+>', ' ')
    $text = [System.Net.WebUtility]::HtmlDecode($text)
    $text = $text.Normalize([Text.NormalizationForm]::FormD)
    $text = [regex]::Replace($text, '\p{Mn}', '')
    $text = $text -replace '[\u2018\u2019]', "'"
    $text = $text -replace '[\u201C\u201D]', '"'
    $text = $text -replace '[\u2013\u2014]', '-'
    $text = $text -replace '[\u2026]', '...'
    $text = [regex]::Replace($text, '[^\x00-\x7F]', '')
    return ([regex]::Replace($text, '\s+', ' ')).Trim()
}

function Get-FirstSentence {
    param([string] $Text)

    $clean = Convert-HtmlText $Text
    $match = [regex]::Match($clean, '^(.+?[.!?])(?:\s|$)')
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    return $clean
}

function Get-CellText {
    param([string] $Cell)

    return (Convert-HtmlText $Cell)
}

function Get-ParentNames {
    param(
        [string] $Html,
        [string[]] $ElementNames,
        [string] $CurrentName
    )

    $text = Convert-HtmlText $Html
    $parents = [System.Collections.Generic.List[string]]::new()
    foreach ($candidate in $ElementNames) {
        if ($candidate -eq $CurrentName) { continue }
        $escaped = [regex]::Escape($candidate)
        $patterns = @(
            "(?:subelement|descendent|descendant|inside|within|child(?: elements?)? of|inside exactly one)\s+(?:an?\s+|the\s+)?$escaped\b",
            "(?:subelement|descendent|descendant)\s+of\s+(?:an?\s+|the\s+)?$escaped\b"
        )
        if ($patterns | Where-Object { $text -match $_ }) {
            $parents.Add($candidate)
        }
    }
    return @($parents | Select-Object -Unique)
}

function Get-Values {
    param(
        [string] $Html,
        [string[]] $Exclude = @()
    )

    $values = [System.Collections.Generic.List[string]]::new()

    # Value tables use the first column for the documented choices or states.
    foreach ($tableMatch in [regex]::Matches($Html, '(?is)<table\b.*?</table>')) {
        $table = $tableMatch.Value
        $header = Convert-HtmlText ([regex]::Match($table, '(?is)<thead\b.*?</thead>').Value)
        if ($header -match '(?i)\bAttribute\b') { continue }
        if ($header -notmatch '(?i)\bValue\b|\bState\b|\bstatus\b') { continue }
        foreach ($row in [regex]::Matches($table, '(?is)<tr\b.*?</tr>')) {
            $cells = @([regex]::Matches($row.Value, '(?is)<t[dh]\b.*?</t[dh]>') | ForEach-Object { Get-CellText $_.Value })
            if ($cells.Count -ge 2 -and $cells[0] -notmatch '^(?i:Value|State|dpiAwareness element status)') {
                if ($cells[0] -and -not $values.Contains($cells[0])) { $values.Add($cells[0]) }
            }
        }
    }

    # Strong/code tokens in prose and lists are how this page documents most values.
    foreach ($paragraph in [regex]::Matches($Html, '(?is)<(?:p|li)\b.*?</(?:p|li)>')) {
        $paragraphText = Convert-HtmlText $paragraph.Value
        if ($paragraphText -notmatch '(?i)allowed|valid value|following value|following values|the value|values include|can contain') {
            continue
        }
        foreach ($token in [regex]::Matches($paragraph.Value, '(?is)<(?:strong|code)\b[^>]*>(.*?)</(?:strong|code)>')) {
            $value = Get-CellText $token.Groups[1].Value
            if ($value -and $value.Length -le 80 -and $value -notmatch '\s' -and
                -not $values.Contains($value) -and $Exclude -notcontains $value) {
                $values.Add($value)
            }
        }
    }
    # GUID literals (supportedOS Id values) are documented inside attribute cells, not tables.
    foreach ($guid in [regex]::Matches($Html, '(?i)\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}')) {
        if (-not $values.Contains($guid.Value)) { $values.Add($guid.Value) }
    }
    return @($values)
}

if ($FromFile) {
    $resolved = Resolve-Path -LiteralPath $FromFile -ErrorAction Stop
    $html = Get-Content -LiteralPath $resolved -Raw
    $source = $resolved.Path
} else {
    $response = Invoke-WebRequest -Uri $sourceUrl -UseBasicParsing
    $html = $response.Content
    $source = $sourceUrl
}

$sectionMatch = [regex]::Match($html, '(?is)<h2\s+id=["'']elements["''][^>]*>.*?</h2>(?<body>.*?)(?=<h2\b|\z)')
if (-not $sectionMatch.Success) {
    throw 'Could not locate the Elements section in the source page.'
}
$section = $sectionMatch.Groups['body'].Value
$headingMatches = @([regex]::Matches($section, '(?is)<h3\s+id=["'']([^"'']+)["''][^>]*>(.*?)</h3>'))
$elementNames = @($headingMatches | ForEach-Object { Convert-HtmlText $_.Groups[2].Value })
$elements = [System.Collections.Generic.List[object]]::new()
$unparsed = [System.Collections.Generic.List[string]]::new()

for ($index = 0; $index -lt $headingMatches.Count; $index++) {
    $heading = $headingMatches[$index]
    $end = if ($index + 1 -lt $headingMatches.Count) { $headingMatches[$index + 1].Index } else { $section.Length }
    $block = $section.Substring($heading.Index + $heading.Length, $end - ($heading.Index + $heading.Length))
    $name = $elementNames[$index]
    $paragraphs = @([regex]::Matches($block, '(?is)<p\b.*?</p>'))
    $description = if ($paragraphs.Count) { Get-FirstSentence $paragraphs[0].Value } else { '' }
    $attributes = [System.Collections.Generic.List[object]]::new()

    foreach ($tableMatch in [regex]::Matches($block, '(?is)<table\b.*?</table>')) {
        $table = $tableMatch.Value
        $header = Convert-HtmlText ([regex]::Match($table, '(?is)<thead\b.*?</thead>').Value)
        if ($header -notmatch '(?i)^Attribute\s+Description$') { continue }
        foreach ($row in [regex]::Matches($table, '(?is)<tbody\b.*?</tbody>')) {
            foreach ($dataRow in [regex]::Matches($row.Value, '(?is)<tr\b.*?</tr>')) {
                $cells = @([regex]::Matches($dataRow.Value, '(?is)<td\b.*?</td>') | ForEach-Object { Get-CellText $_.Value })
                if ($cells.Count -ge 2 -and $cells[0]) {
                    $attributes.Add([pscustomobject]@{
                        name = $cells[0]
                        description = Get-FirstSentence $cells[1]
                    })
                }
            }
        }
    }

    $parents = @(Get-ParentNames $block $elementNames $name)
    $values = @(Get-Values $block ($elementNames + @($attributes | ForEach-Object { $_.name })))
    if (-not $paragraphs.Count -and -not $attributes.Count) { $unparsed.Add($name) }
    $elements.Add([pscustomobject]@{
        name = $name
        parent = if ($parents.Count -eq 1) { $parents[0] } elseif ($parents.Count -gt 1) { $parents } else { $null }
        description = $description
        attributes = @($attributes)
        values = $values
    })
}

# requestedExecutionLevel has no h3 of its own; the page documents it (and its
# level/uiAccess attributes) only in trustInfo prose, so pull sentences from there.
$trustHeading = [regex]::Match($section, '(?is)<h3\s+id=["'']trustinfo["''][^>]*>.*?</h3>')
if ($trustHeading.Success) {
    $trustEnd = [regex]::Match($section.Substring($trustHeading.Index + $trustHeading.Length), '(?is)<h3\s+id=|<h2\b')
    $trustLength = if ($trustEnd.Success) { $trustEnd.Index } else { $section.Length - ($trustHeading.Index + $trustHeading.Length) }
    $trustBlock = $section.Substring($trustHeading.Index + $trustHeading.Length, $trustLength)
    $trustText = Convert-HtmlText $trustBlock
    $requestedAttributes = [System.Collections.Generic.List[object]]::new()
    $levelDesc = [regex]::Match($trustText, '[^.!?]*\bthe level attribute\b[^.!?]*[.!?]').Value.Trim()
    if ($levelDesc) { $requestedAttributes.Add([pscustomobject]@{ name = 'level'; description = $levelDesc }) }
    $uiDesc = [regex]::Match($trustText, '[^.!?]*\boptional attribute uiAccess\b[^.!?]*[.!?]').Value.Trim()
    if ($uiDesc) { $requestedAttributes.Add([pscustomobject]@{ name = 'uiAccess'; description = $uiDesc }) }
    $requestedValues = @(Get-Values $trustBlock ($elementNames + @('level', 'uiAccess', 'requestedExecutionLevel')))
    $elements.Add([pscustomobject]@{
        name = 'requestedExecutionLevel'
        parent = 'trustInfo'
        description = [regex]::Match($trustText, 'Requested execution levels\b[^.!?]*[.!?]').Value.Trim()
        attributes = @($requestedAttributes)
        values = $requestedValues
    })
    # The same prose sits under trustInfo's h3, so strip the child's tokens from the parent.
    $trustElement = $elements | Where-Object { $_.name -eq 'trustInfo' }
    if ($trustElement) {
        $strip = $requestedValues + @('level', 'uiAccess', 'requestedExecutionLevel')
        $trustElement.values = @($trustElement.values | Where-Object { $strip -notcontains $_ })
    }
}

$inventory = [pscustomobject]@{
    source = $source
    sourceUrl = $sourceUrl
    elements = @($elements)
}
$attributeCount = @($elements | ForEach-Object { $_.attributes }).Count
$valueCount = @($elements | ForEach-Object { $_.values }).Count

if ($OutJson) {
    $outParent = Split-Path -Parent $OutJson
    if ($outParent -and -not (Test-Path -LiteralPath $outParent)) {
        New-Item -ItemType Directory -Path $outParent -Force | Out-Null
    }
    $inventory | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutJson -Encoding utf8
}

Write-Output ("Source: {0}" -f $source)
Write-Output ("Elements: {0}" -f $elements.Count)
Write-Output ("Attributes: {0}" -f $attributeCount)
Write-Output ("Enumerable values: {0}" -f $valueCount)
foreach ($element in $elements) {
    Write-Output ("{0}: {1} attributes" -f $element.name, @($element.attributes).Count)
}
if ($unparsed.Count) {
    Write-Output ("Could not parse cleanly: {0}" -f ($unparsed -join ', '))
} else {
    Write-Output 'Could not parse cleanly: none'
}
