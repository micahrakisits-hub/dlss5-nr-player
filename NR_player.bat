@echo off
cd /d "%~dp0"
set "PATH=%~dp0;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName System.Windows.Forms; $d=New-Object System.Windows.Forms.OpenFileDialog; $d.Filter='Video (*.mp4;*.mkv;*.avi;*.mov;*.webm;*.ts;*.m2ts;*.flv;*.wmv;*.m4v)|*.mp4;*.mkv;*.avi;*.mov;*.webm;*.ts;*.m2ts;*.flv;*.wmv;*.m4v|All files (*.*)|*.*'; $d.Title='Select video for DLSS5 NR'; if($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){ & '.\nr_player.exe' $d.FileName }"
pause
