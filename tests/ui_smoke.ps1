param([switch]$KeepOpen,[string]$Executable='build\chat.exe',[int]$ModelServerPort=0)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UiCheck {
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 public delegate bool EnumProc(IntPtr window,IntPtr state);
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback,IntPtr state);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint process);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window,System.Text.StringBuilder text,int size);
 [DllImport("user32.dll",EntryPoint="GetDlgItem")] public static extern IntPtr NativeGetDlgItem(IntPtr window,int id);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr window,EnumProc callback,IntPtr state);
 [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
 [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr window);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
 [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access,bool inherit,uint id);
 [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr process,IntPtr address,UIntPtr bytes,uint type,uint protect);
 [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr process,IntPtr address,byte[] bytes,UIntPtr length,out UIntPtr written);
 [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr process,IntPtr address,UIntPtr bytes,uint type);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
 public static void DropFiles(IntPtr window,string[] paths) {
  var names=System.Text.Encoding.Unicode.GetBytes(string.Join("\0",paths)+"\0\0");
  // WM_DROPFILES does not marshal pointers between processes. Put the fixture
  // DROPFILES structure in the test app's address space, as the shell does.
  var data=new byte[20+names.Length];data[0]=20;data[16]=1;Array.Copy(names,0,data,20,names.Length);
  uint id;GetWindowThreadProcessId(window,out id);var process=OpenProcess(0x28,false,id);
  var memory=VirtualAllocEx(process,IntPtr.Zero,(UIntPtr)data.Length,0x3000,4);
  try {UIntPtr written;if(memory==IntPtr.Zero||!WriteProcessMemory(process,memory,data,(UIntPtr)data.Length,out written))throw new Exception("Could not prepare drop fixture");SendMessage(window,0x233,memory,IntPtr.Zero);}
  finally {if(memory!=IntPtr.Zero)VirtualFreeEx(process,memory,UIntPtr.Zero,0x8000);CloseHandle(process);}
 }
 public static IntPtr GetDlgItem(IntPtr window,int id) {
  IntPtr found=NativeGetDlgItem(window,id);if(found!=IntPtr.Zero)return found;
  EnumChildWindows(window,(child,state)=>{if(GetDlgCtrlID(child)==id){found=child;return false;}return true;},IntPtr.Zero);return found;
 }
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out Rect rect);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window,out Rect rect);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window,uint message,IntPtr wParam,IntPtr lParam);
 [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr window);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window,uint message,IntPtr wParam,IntPtr lParam);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window,IntPtr after,int x,int y,int width,int height,uint flags);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window,int command);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
 [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr window,int attribute,out int value,int size);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)] public static extern IntPtr SetText(IntPtr window,uint message,IntPtr parameter,string text);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)] public static extern IntPtr GetText(IntPtr window,uint message,IntPtr size,System.Text.StringBuilder text);
 public static IntPtr Find(int process) {
  IntPtr found=IntPtr.Zero;
  EnumWindows((window,state)=>{uint id;GetWindowThreadProcessId(window,out id);var name=new System.Text.StringBuilder(256);GetClassName(window,name,256);if(id==process&&name.ToString()=="ChatHarness"){found=window;return false;}return true;},IntPtr.Zero);
  return found;
 }
 public static IntPtr FindDialog(int process,int control=0) {
  IntPtr found=IntPtr.Zero;
  EnumWindows((window,state)=>{uint id;GetWindowThreadProcessId(window,out id);var name=new System.Text.StringBuilder(256);GetClassName(window,name,256);if(id==process&&name.ToString()=="#32770"&&(control==0||GetDlgItem(window,control)!=IntPtr.Zero)){found=window;return false;}return true;},IntPtr.Zero);
  return found;
 }
}
'@
$null=[UiCheck]::SetThreadDpiAwarenessContext([IntPtr](-4))
$root=Split-Path $PSScriptRoot -Parent
function AssertModalChrome([IntPtr]$modal) {
 $bounds=New-Object UiCheck+Rect;$client=New-Object UiCheck+Rect;$closeBounds=New-Object UiCheck+Rect
 $null=[UiCheck]::GetWindowRect($modal,[ref]$bounds);$null=[UiCheck]::GetClientRect($modal,[ref]$client)
 $close=[UiCheck]::GetDlgItem($modal,12)
 if($close -eq [IntPtr]::Zero){throw 'Modal custom close button missing.'}
 $null=[UiCheck]::GetWindowRect($close,[ref]$closeBounds)
 $header=[int][Math]::Round(48*[UiCheck]::GetDpiForWindow($modal)/96)
 if($closeBounds.Right-$closeBounds.Left -ne $header -or $closeBounds.Bottom-$closeBounds.Top -ne $header -or $closeBounds.Top -ne $bounds.Top -or $closeBounds.Right -ne $bounds.Right){throw 'Modal close control must be a square aligned with the custom header.'}
 if($client.Right -ne $bounds.Right-$bounds.Left -or $client.Bottom -ne $bounds.Bottom-$bounds.Top){throw 'Modal still has a native frame.'}
 $corner=0
 if([UiCheck]::DwmGetWindowAttribute($modal,33,[ref]$corner,4) -eq 0 -and $corner -ne 1){throw 'Modal corners must be square.'}
 $position=($bounds.Left+24) -bor (($bounds.Top+24) -shl 16)
 if([UiCheck]::SendMessage($modal,0x84,[IntPtr]::Zero,[IntPtr]$position).ToInt32() -ne 2){throw 'Modal header must support dragging.'}
}
function SaveWindowPreview([IntPtr]$target,[string]$name) {
 $bounds=New-Object UiCheck+Rect;$null=[UiCheck]::GetClientRect($target,[ref]$bounds)
 $bitmap=New-Object Drawing.Bitmap($bounds.Right,$bounds.Bottom);$graphics=[Drawing.Graphics]::FromImage($bitmap)
 $dc=$graphics.GetHdc()
 try{$null=[UiCheck]::PrintWindow($target,$dc,3)}finally{$graphics.ReleaseHdc($dc)}
 $bitmap.Save((Join-Path $root "build\$name"),[Drawing.Imaging.ImageFormat]::Png)
 $graphics.Dispose();$bitmap.Dispose()
}
if($ModelServerPort){
 $isolated=Join-Path $root 'build\ui-settings-test'
 $null=New-Item -ItemType Directory -Force -Path $isolated
 Copy-Item -LiteralPath (Join-Path $root $Executable) -Destination (Join-Path $isolated 'chat.exe') -Force
 Set-Content -LiteralPath (Join-Path $isolated '.env') -Value ''
 Remove-Item -LiteralPath (Join-Path $isolated 'provider-settings.json') -ErrorAction SilentlyContinue
 Remove-Item -LiteralPath (Join-Path $isolated 'chat-history.json') -ErrorAction SilentlyContinue
 $Executable='build\ui-settings-test\chat.exe'
}
$process=Start-Process -FilePath (Join-Path $root $Executable) -WindowStyle Hidden -PassThru
$window=[IntPtr]::Zero
try {
 $null=$process.WaitForInputIdle(5000)
 $window=[UiCheck]::Find($process.Id)
 if($window -eq [IntPtr]::Zero){throw 'App window not created.'}
 if(![UiCheck]::IsZoomed($window)){throw 'App must launch maximized.'}
 $toggle=[UiCheck]::GetDlgItem($window,4)
 if($toggle -eq [IntPtr]::Zero){throw 'Sidebar toggle missing.'}
 $expandedNav=New-Object UiCheck+Rect;$expandedInput=New-Object UiCheck+Rect
 $null=[UiCheck]::GetWindowRect([UiCheck]::GetDlgItem($window,1),[ref]$expandedNav)
 $null=[UiCheck]::GetWindowRect([UiCheck]::GetDlgItem($window,3),[ref]$expandedInput)
 $toggleBounds=New-Object UiCheck+Rect;$null=[UiCheck]::GetWindowRect($toggle,[ref]$toggleBounds)
 $compactSize=[int][Math]::Round(40*[UiCheck]::GetDpiForWindow($window)/96)
 $toggleInset=[int][Math]::Round(16*[UiCheck]::GetDpiForWindow($window)/96)
 if($toggleBounds.Right-$toggleBounds.Left -ne $compactSize -or $toggleBounds.Bottom-$toggleBounds.Top -ne $compactSize -or $toggleBounds.Right -ne $expandedNav.Right-$toggleInset -or $toggleBounds.Bottom -gt $expandedNav.Top){throw 'Sidebar toggle must be a compact square aligned to the right above New chat.'}
 $null=[UiCheck]::SendMessage($toggle,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
 $collapsedNav=New-Object UiCheck+Rect;$collapsedInput=New-Object UiCheck+Rect
 $null=[UiCheck]::GetWindowRect([UiCheck]::GetDlgItem($window,1),[ref]$collapsedNav)
 $null=[UiCheck]::GetWindowRect([UiCheck]::GetDlgItem($window,3),[ref]$collapsedInput)
 $railWidth=[int][Math]::Round(72*[UiCheck]::GetDpiForWindow($window)/96)
 if($collapsedNav.Right-$collapsedNav.Left -ne $railWidth){throw 'Collapsed sidebar must be a 72 DIP icon rail.'}
 if($collapsedInput.Left -ge $expandedInput.Left -or $collapsedInput.Right-$collapsedInput.Left -lt $expandedInput.Right-$expandedInput.Left){throw 'Collapsed sidebar must give the conversation more space.'}
 SaveWindowPreview $window 'sidebar-collapsed-preview.png'
 $settings=[UiCheck]::GetDlgItem($window,2)
 if($settings -eq [IntPtr]::Zero){throw 'Settings button missing.'}
 $null=[UiCheck]::PostMessage($window,0x111,[IntPtr]2,$settings)
 $dialog=[IntPtr]::Zero
 for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$dialog=[UiCheck]::FindDialog($process.Id);if($dialog -ne [IntPtr]::Zero){break}}
 if($dialog -eq [IntPtr]::Zero -or [UiCheck]::IsWindowEnabled($window)){throw 'Settings must open as a modal.'}
 AssertModalChrome $dialog
 foreach($id in @(101,102,103,105,106,108,109,1,2)){if([UiCheck]::GetDlgItem($dialog,$id) -eq [IntPtr]::Zero){throw "Settings control $id missing."}}
 if([UiCheck]::SendMessage([UiCheck]::GetDlgItem($dialog,102),0xd2,[IntPtr]::Zero,[IntPtr]::Zero) -eq [IntPtr]::Zero){throw 'API key must be masked.'}
 if($ModelServerPort){
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,101),12,[IntPtr]::Zero,"http://127.0.0.1:$ModelServerPort/v1/chat/completions")
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,102),12,[IntPtr]::Zero,'test-key')
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,101),12,[IntPtr]::Zero,'invalid-url')
  $null=[UiCheck]::PostMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero)
  $alert=[IntPtr]::Zero
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$alert=[UiCheck]::FindDialog($process.Id,301);if($alert -ne [IntPtr]::Zero){break}}
  if($alert -eq [IntPtr]::Zero){throw 'Settings validation dialog missing.'}
  AssertModalChrome $alert
  $null=[UiCheck]::SendMessage([UiCheck]::GetDlgItem($alert,12),0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
  if(![UiCheck]::IsWindowEnabled($dialog)){throw 'Alert close button must restore the settings modal.'}
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,101),12,[IntPtr]::Zero,"http://127.0.0.1:$ModelServerPort/v1/chat/completions")
  $models=[UiCheck]::GetDlgItem($dialog,103)
  for($attempt=0;$attempt -lt 100;$attempt++){Start-Sleep -Milliseconds 100;if([UiCheck]::SendMessage($models,0x146,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64() -eq 2){break}}
  if($attempt -eq 100){throw 'Models did not populate after entering a key.'}
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,108),12,[IntPtr]::Zero,'12')
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,109),12,[IntPtr]::Zero,'40')
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]110,[IntPtr]::Zero)
  $promptEditor=[UiCheck]::GetDlgItem($dialog,501)
  if(![UiCheck]::IsWindowVisible($promptEditor)){throw 'System prompt editor must be visible.'}
  $customPrompt='Custom instructions'+[Environment]::NewLine+'Second line'
  $null=[UiCheck]::SetText($promptEditor,12,[IntPtr]::Zero,$customPrompt)
  SaveWindowPreview $dialog 'system-prompt-preview.png'
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]105,[IntPtr]::Zero)
  SaveWindowPreview $dialog 'settings-preview.png'
  $null=[UiCheck]::SendMessage($models,0x14e,[IntPtr]1,[IntPtr]::Zero)
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero)
  if(!(Test-Path -LiteralPath (Join-Path $isolated 'provider-settings.json'))){throw 'Selected model was not saved.'}
  $null=[UiCheck]::PostMessage($window,0x111,[IntPtr]2,$settings)
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$dialog=[UiCheck]::FindDialog($process.Id);if($dialog -ne [IntPtr]::Zero){break}}
  $models=[UiCheck]::GetDlgItem($dialog,103)
  for($attempt=0;$attempt -lt 100;$attempt++){Start-Sleep -Milliseconds 100;if([UiCheck]::SendMessage($models,0x146,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64() -eq 2){break}}
  $selected=New-Object System.Text.StringBuilder 512
  $null=[UiCheck]::GetText($models,13,[IntPtr]512,$selected)
  if($selected.ToString() -ne 'z/model'){throw 'Model selection was not restored after reopening settings.'}
  $promptText=New-Object System.Text.StringBuilder 4096
  $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($dialog,501),13,[IntPtr]4096,$promptText)
  if($promptText.ToString() -ne $customPrompt){throw 'System prompt did not persist.'}
  $promptPage=[UiCheck]::GetParent([UiCheck]::GetDlgItem($dialog,501))
  $null=[UiCheck]::SendMessage($promptPage,0x111,[IntPtr]502,[IntPtr]::Zero)
  $defaultText=New-Object System.Text.StringBuilder 8192
  $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($dialog,501),13,[IntPtr]8192,$defaultText)
  if(!$defaultText.ToString().StartsWith('You are a coding assistant')){throw 'Restore default prompt failed.'}
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,501),12,[IntPtr]::Zero,$customPrompt)
  foreach($item in @(@(108,'12'),@(109,'40'))) {
   $limit=New-Object System.Text.StringBuilder 32
   $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($dialog,$item[0]),13,[IntPtr]32,$limit)
   if($limit.ToString() -ne $item[1]){throw 'Run limit was not restored after reopening settings.'}
  }
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,103),12,[IntPtr]::Zero,'test/model')
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]106,[IntPtr]::Zero)
  if(![UiCheck]::IsWindowVisible([UiCheck]::GetDlgItem($dialog,201)) -or [UiCheck]::IsWindowVisible([UiCheck]::GetDlgItem($dialog,101))){throw 'MCP tab must replace Provider in the same modal.'}
  $mcpDialog=$dialog
  $mcpPage=[UiCheck]::GetParent([UiCheck]::GetDlgItem($dialog,201))
  $mcpJson=@{mcpServers=@{fixture=@{url="http://127.0.0.1:$ModelServerPort/mcp";headers=@{Authorization='Bearer mcp-token'}}}} | ConvertTo-Json -Depth 10
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($mcpDialog,201),12,[IntPtr]::Zero,$mcpJson)
  $null=[UiCheck]::SendMessage($mcpPage,0x111,[IntPtr]203,[IntPtr]::Zero)
  for($attempt=0;$attempt -lt 100;$attempt++){
   Start-Sleep -Milliseconds 100
   $status=New-Object System.Text.StringBuilder 16384
   $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($mcpDialog,202),13,[IntPtr]16384,$status)
   if($status.ToString().Contains('connected (1 tools)')){break}
  }
  if($attempt -eq 100){throw 'MCP connection test did not discover the fixture tool.'}
  $null=[UiCheck]::SendMessage($mcpPage,0x111,[IntPtr]205,[IntPtr]::Zero)
  $disabledText=New-Object System.Text.StringBuilder 16384
  $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($dialog,201),13,[IntPtr]16384,$disabledText)
  $disabledConfig=$disabledText.ToString() | ConvertFrom-Json
  if($disabledConfig.enabled -ne $false -or !$disabledConfig.mcpServers.fixture){throw 'Disable must preserve the server configuration.'}
  $null=[UiCheck]::SendMessage($mcpPage,0x111,[IntPtr]203,[IntPtr]::Zero)
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$status=New-Object System.Text.StringBuilder 16384;$null=[UiCheck]::GetText([UiCheck]::GetDlgItem($dialog,202),13,[IntPtr]16384,$status);if($status.ToString().Contains('No enabled MCP servers')){break}}
  if($attempt -eq 50){throw 'Disabled MCP must not connect.'}
  $null=[UiCheck]::SendMessage($mcpPage,0x111,[IntPtr]205,[IntPtr]::Zero)
  SaveWindowPreview $dialog 'mcp-settings-preview.png'
  $null=[UiCheck]::SendMessage($mcpDialog,0x111,[IntPtr]1,[IntPtr]::Zero)
  if(!(Test-Path -LiteralPath (Join-Path $isolated 'mcp-servers.json'))){throw 'MCP servers were not saved.'}
  $null=[UiCheck]::PostMessage($window,0x111,[IntPtr]2,$settings)
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$dialog=[UiCheck]::FindDialog($process.Id,101);if($dialog -ne [IntPtr]::Zero){break}}
  $persistedModel=New-Object System.Text.StringBuilder 512;$null=[UiCheck]::GetText([UiCheck]::GetDlgItem($dialog,103),13,[IntPtr]512,$persistedModel)
  if($persistedModel.ToString() -ne 'test/model'){throw 'Save must retain edits made across both tabs.'}
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]106,[IntPtr]::Zero)
  $mcpDialog=$dialog;$mcpPage=[UiCheck]::GetParent([UiCheck]::GetDlgItem($dialog,201))
  $null=[UiCheck]::PostMessage($mcpPage,0x111,[IntPtr]204,[IntPtr]::Zero)
  $confirmation=[IntPtr]::Zero
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$confirmation=[UiCheck]::FindDialog($process.Id,301);if($confirmation -ne [IntPtr]::Zero){break}}
  if($confirmation -eq [IntPtr]::Zero){throw 'MCP replacement confirmation missing.'}
  AssertModalChrome $confirmation
  $null=[UiCheck]::SendMessage([UiCheck]::GetDlgItem($confirmation,12),0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
  $restoredMcp=New-Object System.Text.StringBuilder 16384
  $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($mcpDialog,201),13,[IntPtr]16384,$restoredMcp)
  if(!$restoredMcp.ToString().Contains('fixture')){throw 'MCP servers were not restored.'}
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]105,[IntPtr]::Zero)
  if(![UiCheck]::IsWindowVisible([UiCheck]::GetDlgItem($dialog,101)) -or [UiCheck]::IsWindowVisible([UiCheck]::GetDlgItem($dialog,201))){throw 'Provider tab must restore its panel.'}
 }
 $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]107,[IntPtr]::Zero)
 $testButton=[UiCheck]::GetDlgItem($dialog,401)
 if($testButton -eq [IntPtr]::Zero -or ![UiCheck]::IsWindowVisible($testButton)){throw 'Computer Control tab is missing its access test.'}
 $null=[UiCheck]::SendMessage($testButton,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
 for($attempt=0;$attempt -lt 50;$attempt++) {
  Start-Sleep -Milliseconds 100
  $access=New-Object System.Text.StringBuilder 1024
  $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($dialog,403),13,[IntPtr]1024,$access)
  if([UiCheck]::IsWindowEnabled($testButton)){break}
 }
 if(!$access.ToString().StartsWith('Access works:')){throw "Computer access test failed: $access"}
 $circle=[UiCheck]::GetDlgItem($dialog,404)
 if($circle -eq [IntPtr]::Zero){throw 'Mouse circle button missing.'}
 $null=[UiCheck]::SendMessage($circle,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
 for($attempt=0;$attempt -lt 100;$attempt++) {
  Start-Sleep -Milliseconds 100
  if([UiCheck]::IsWindowEnabled($circle)){break}
 }
 $circleStatus=New-Object System.Text.StringBuilder 1024
 $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($dialog,403),13,[IntPtr]1024,$circleStatus)
 if(!$circleStatus.ToString().StartsWith('Mouse circle passed:')){throw "Mouse circle test failed: $circleStatus"}
 SaveWindowPreview $dialog 'computer-settings-preview.png'
 $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]2,[IntPtr]::Zero)
 if(![UiCheck]::IsWindowEnabled($window)){throw 'Cancel must restore the main window.'}
 $null=[UiCheck]::SendMessage($window,0x111,[IntPtr]0x10004,[IntPtr]::Zero)
 $restoredNav=New-Object UiCheck+Rect;$restoredInput=New-Object UiCheck+Rect
 $null=[UiCheck]::GetWindowRect([UiCheck]::GetDlgItem($window,1),[ref]$restoredNav)
 $null=[UiCheck]::GetWindowRect([UiCheck]::GetDlgItem($window,3),[ref]$restoredInput)
 if($restoredNav.Right -ne $expandedNav.Right -or $restoredInput.Left -ne $expandedInput.Left){throw 'Expanded sidebar must restore the original layout.'}
 $inputWindow=[UiCheck]::GetDlgItem($window,3)
 function InputHeight {
  $r=New-Object UiCheck+Rect
  $null=[UiCheck]::GetClientRect($inputWindow,[ref]$r)
  return $r.Bottom-$r.Top
 }
 $singleLine=InputHeight
 $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,'Short prompt')
 if((InputHeight) -ne $singleLine){throw 'Short input must remain one line.'}
 $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,"First line`r`nSecond line")
 if((InputHeight) -ne 2*$singleLine){throw 'Manual newline must grow the prompt.'}
 $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,('A long prompt that wraps naturally. '*100))
 if((InputHeight) -le $singleLine -or (InputHeight) -gt 6*$singleLine){throw 'Wrapped input must grow up to six lines.'}
 $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,'Short again')
 if((InputHeight) -ne $singleLine){throw 'Prompt must shrink after text is shortened.'}
 $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,'')
 if((InputHeight) -ne $singleLine){throw 'Cleared prompt must return to one line.'}
 $expected=[int][Math]::Round(48*[UiCheck]::GetDpiForWindow($window)/96)
 foreach($id in @(10,11,12)){
  $button=[UiCheck]::GetDlgItem($window,$id)
  $rect=New-Object UiCheck+Rect
  $null=[UiCheck]::GetWindowRect($button,[ref]$rect)
  if($rect.Right-$rect.Left -ne $expected -or $rect.Bottom-$rect.Top -ne $expected){throw "Caption button $id is not a 48 DIP square."}
 }
 $maximize=[UiCheck]::GetDlgItem($window,11)
 $null=[UiCheck]::SendMessage($maximize,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
 if([UiCheck]::IsZoomed($window)){throw 'Restore button failed.'}
 $null=[UiCheck]::SetWindowPos($window,[IntPtr]::Zero,60,60,1120,820,0x14)
 Start-Sleep -Milliseconds 150
 $narrowWidth=[int][Math]::Round(360*[UiCheck]::GetDpiForWindow($window)/96)
 $null=[UiCheck]::SetWindowPos($window,[IntPtr]::Zero,60,60,$narrowWidth,820,0x14)
 $narrowInput=New-Object UiCheck+Rect
 $null=[UiCheck]::GetWindowRect($inputWindow,[ref]$narrowInput)
 if($narrowInput.Right -le $narrowInput.Left){throw 'Narrow window must retain a usable input.'}
 $null=[UiCheck]::SetWindowPos($window,[IntPtr]::Zero,60,60,1120,820,0x14)
 $client=New-Object UiCheck+Rect;$null=[UiCheck]::GetClientRect($window,[ref]$client)
 $bitmap=New-Object Drawing.Bitmap($client.Right,$client.Bottom)
 $graphics=[Drawing.Graphics]::FromImage($bitmap)
 $dc=$graphics.GetHdc()
 try{$null=[UiCheck]::PrintWindow($window,$dc,3)}finally{$graphics.ReleaseHdc($dc)}
 $bitmap.Save((Join-Path $root 'build\ui-preview.png'),[Drawing.Imaging.ImageFormat]::Png)
 $graphics.Dispose();$bitmap.Dispose()
 if($ModelServerPort){
  $null=[UiCheck]::PostMessage($window,0x111,[IntPtr]2,$settings)
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$dialog=[UiCheck]::FindDialog($process.Id,101);if($dialog -ne [IntPtr]::Zero){break}}
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,101),12,[IntPtr]::Zero,"http://127.0.0.1:$ModelServerPort/markdown/chat/completions")
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,103),12,[IntPtr]::Zero,'test/model')
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero)
  $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,'Render Markdown')
  $null=[UiCheck]::SendMessage($inputWindow,0x100,[IntPtr]13,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 1500
  SaveWindowPreview $window 'markdown-preview.png'
  $null=[UiCheck]::SendMessage($window,0x20a,[IntPtr](960 -shl 16),[IntPtr]::Zero)
  Start-Sleep -Milliseconds 100
  SaveWindowPreview $window 'markdown-preview-top.png'
  $null=[UiCheck]::SendMessage([UiCheck]::GetDlgItem($window,1),0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
  $null=[UiCheck]::PostMessage($window,0x111,[IntPtr]2,$settings)
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$dialog=[UiCheck]::FindDialog($process.Id,101);if($dialog -ne [IntPtr]::Zero){break}}
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,101),12,[IntPtr]::Zero,"http://127.0.0.1:$ModelServerPort/agent-formats")
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero)
  $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,'Show structured tool outputs')
  $null=[UiCheck]::SendMessage($inputWindow,0x100,[IntPtr]13,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 2000
  SaveWindowPreview $window 'tool-formats-preview.png'
 }
 if($ModelServerPort){
  $null=[UiCheck]::PostMessage($window,0x111,[IntPtr]2,$settings)
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$dialog=[UiCheck]::FindDialog($process.Id,101);if($dialog -ne [IntPtr]::Zero){break}}
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,101),12,[IntPtr]::Zero,"http://127.0.0.1:$ModelServerPort/attachments/chat/completions")
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero)
  $document=Join-Path $isolated 'dropped.txt';$image=Join-Path $isolated 'dropped.png'
  [IO.File]::WriteAllText($document,'Dropped document fixture')
  [IO.File]::WriteAllBytes($image,[Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1sAAAAASUVORK5CYII='))
  $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,'')
  [UiCheck]::DropFiles($inputWindow,@($document,$image))
  $clear=[UiCheck]::GetDlgItem($window,6)
  if(![UiCheck]::IsWindowVisible($clear)){throw 'Dropped files must appear in the composer.'}
  SaveWindowPreview $window 'pending-attachments-preview.png'
  $null=[UiCheck]::SendMessage([UiCheck]::GetDlgItem($window,5),0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 700
  if([UiCheck]::IsWindowVisible($clear)){throw 'Sent attachments must leave the draft.'}
  SaveWindowPreview $window 'attachments-history-preview.png'
  [UiCheck]::DropFiles($window,@($document))
  if(![UiCheck]::IsWindowVisible($clear)){throw 'Window-level drops must queue attachments.'}
  $null=[UiCheck]::SendMessage($clear,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
  if([UiCheck]::IsWindowVisible($clear)){throw 'Remove attachments must clear the pending files.'}
  $null=[UiCheck]::PostMessage($window,0x111,[IntPtr]2,$settings)
  for($attempt=0;$attempt -lt 50;$attempt++){Start-Sleep -Milliseconds 100;$dialog=[UiCheck]::FindDialog($process.Id,101);if($dialog -ne [IntPtr]::Zero){break}}
  $null=[UiCheck]::SetText([UiCheck]::GetDlgItem($dialog,101),12,[IntPtr]::Zero,"http://127.0.0.1:$ModelServerPort/slow/chat/completions")
  $null=[UiCheck]::SendMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero)
  $send=[UiCheck]::GetDlgItem($window,5)
  if($send -eq [IntPtr]::Zero){throw 'Send button missing.'}
  foreach($iteration in 1..2){
   $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,'Send and cancel from the composer')
   $null=[UiCheck]::SendMessage($send,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
   $label=New-Object System.Text.StringBuilder 128
   $null=[UiCheck]::GetText($send,13,[IntPtr]128,$label)
   if($label.ToString() -ne 'Stop agent'){throw 'Send must switch to Stop while running.'}
   Start-Sleep -Milliseconds 150
   $null=[UiCheck]::SetText($inputWindow,12,[IntPtr]::Zero,'Keep this draft')
   $null=[UiCheck]::SendMessage($send,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
   $label.Clear()|Out-Null;$null=[UiCheck]::GetText($send,13,[IntPtr]128,$label)
   if($label.ToString() -ne 'Send message'){throw 'Stop must restore Send.'}
   $draft=New-Object System.Text.StringBuilder 128
   $null=[UiCheck]::GetText($inputWindow,13,[IntPtr]128,$draft)
   if($draft.ToString() -ne 'Keep this draft'){throw 'Stop must preserve the draft.'}
  }
  SaveWindowPreview $window 'send-stop-preview.png'
 }
 if($ModelServerPort){
  $historyPath=Join-Path $isolated 'chat-history.json'
  if(!(Test-Path -LiteralPath $historyPath)){throw 'Chat history JSON was not created.'}
  $before=Get-Content -LiteralPath $historyPath -Raw | ConvertFrom-Json
  $previousId=$before.active_chat_id
  $null=[UiCheck]::SendMessage([UiCheck]::GetDlgItem($window,1),0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
  $after=Get-Content -LiteralPath $historyPath -Raw | ConvertFrom-Json
  if($after.active_chat_id -eq $previousId -or $after.active_chat_id -notmatch '^[A-Za-z0-9]{5}$'){throw 'New chat needs a distinct five-character ID.'}
  if(!(Test-Path -LiteralPath (Join-Path $isolated ('.workspace/'+$after.active_chat_id)))){throw 'Chat workspace folder missing.'}
  $list=[UiCheck]::GetDlgItem($window,7)
  $null=[UiCheck]::SendMessage($list,0x186,[IntPtr]1,[IntPtr]::Zero)
  $null=[UiCheck]::SendMessage($window,0x111,[IntPtr]0x10007,$list)
  $selectedHistory=Get-Content -LiteralPath $historyPath -Raw | ConvertFrom-Json
  if($selectedHistory.active_chat_id -ne $previousId){throw 'Selecting history must restore the old chat.'}
  $restoredDraft=New-Object System.Text.StringBuilder 512
  $null=[UiCheck]::GetText($inputWindow,13,[IntPtr]512,$restoredDraft)
  if($restoredDraft.ToString() -ne 'Keep this draft'){throw 'Chat selection must restore its draft.'}
  $wide=[int][Math]::Round(900*[UiCheck]::GetDpiForWindow($window)/96)
  $null=[UiCheck]::SetWindowPos($window,[IntPtr]::Zero,60,60,$wide,1200,0x14)
  if(![UiCheck]::IsWindowVisible($list)){throw 'Expanded sidebar must show chat history.'}
  SaveWindowPreview $window 'chat-history-preview.png'
 }
 $minimize=[UiCheck]::GetDlgItem($window,10)
 $null=[UiCheck]::SendMessage($minimize,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
 if(![UiCheck]::IsIconic($window)){throw 'Minimize button failed.'}
 $null=[UiCheck]::ShowWindow($window,9)
 $null=[UiCheck]::SendMessage($maximize,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
 if(![UiCheck]::IsZoomed($window)){throw 'Maximize button failed.'}
 if($ModelServerPort){
  $null=[UiCheck]::SendMessage([UiCheck]::GetDlgItem($window,12),0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
  if(!$process.WaitForExit(5000)){throw 'Chat app did not close for history restore test.'}
  $process=Start-Process -FilePath (Join-Path $root $Executable) -WindowStyle Hidden -PassThru
  $null=$process.WaitForInputIdle(5000)
  $window=[UiCheck]::Find($process.Id)
  if($window -eq [IntPtr]::Zero){throw 'Chat app did not reopen.'}
  $restoredDraft=New-Object System.Text.StringBuilder 512
  $null=[UiCheck]::GetText([UiCheck]::GetDlgItem($window,3),13,[IntPtr]512,$restoredDraft)
  if($restoredDraft.ToString() -ne 'Keep this draft'){throw 'Restart must restore the active chat draft.'}
  $restoredHistory=Get-Content -LiteralPath $historyPath -Raw | ConvertFrom-Json
  if($restoredHistory.active_chat_id -ne $previousId){throw 'Restart must restore the active chat.'}
 }
 Write-Output 'UI passed: settings, chat history restoration, per-chat workspaces, prompt layout, and window controls.'
} finally {
 if(!$KeepOpen -and $window -ne [IntPtr]::Zero){
  $close=[UiCheck]::GetDlgItem($window,12)
  $null=[UiCheck]::SendMessage($close,0xf5,[IntPtr]::Zero,[IntPtr]::Zero)
  if(!$process.WaitForExit(5000)){throw 'Close button failed.'}
  if($process.ExitCode -ne 0){throw 'App exited with an error.'}
 }
}
