$ErrorActionPreference = 'Continue'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CetProbe {
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool IsUserCetAvailableInEnvironment(uint environment);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool GetProcessMitigationPolicy(IntPtr process, int policy, out ulong buffer, UIntPtr length);
  [DllImport("kernel32.dll")]
  public static extern IntPtr GetCurrentProcess();
}
'@
$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$cs = Get-CimInstance Win32_ComputerSystem
$ubr = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').UBR
Write-Host "probe: os='$($os.Caption)' version=$($os.Version).$ubr"
Write-Host "probe: cpu='$($cpu.Name)' manufacturer=$($cpu.Manufacturer) id=$env:PROCESSOR_IDENTIFIER"
Write-Host "probe: model='$($cs.Manufacturer) $($cs.Model)' hypervisor=$($cs.HypervisorPresent)"
$avail = $false
try { $avail = [CetProbe]::IsUserCetAvailableInEnvironment(0) } catch { Write-Host "probe: IsUserCetAvailableInEnvironment error $_" }
Write-Host "probe: user_cet_available=$avail"
$buf = [uint64]0
$ok = [CetProbe]::GetProcessMitigationPolicy([CetProbe]::GetCurrentProcess(), 15, [ref]$buf, [UIntPtr]::new(4))
Write-Host ("probe: pwsh_user_shadow_stack_policy ok={0} flags=0x{1:x}" -f $ok, $buf)
try { Get-ProcessMitigation -System | Select-Object -ExpandProperty UserShadowStack | Format-List | Out-String | Write-Host } catch { Write-Host "probe: Get-ProcessMitigation error $_" }
$dg = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction SilentlyContinue
if ($dg) { Write-Host "probe: vbs_status=$($dg.VirtualizationBasedSecurityStatus) services_running=$($dg.SecurityServicesRunning -join ',')" }
