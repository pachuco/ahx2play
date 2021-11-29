@echo off
call getcomp.bat gcc9


set opts=-std=c99 -mconsole -Wall -Wextra -g
set opts=-std=c99 -mconsole -Wall -Wextra -O3 -s
set opts=%opts% -Wl,--enable-stdcall-fixup -static-libgcc
set linkinc=-lwinmm

set files=.\ahx2play\ahx2play.c .\ahx2play\posix.c .\ahx2play\driver_winmm.c
set files=%files% .\replayer.c .\song.c .\mixer.c
set errlog=.\ahx2play_err.log
set out=C:\p_files\prog\_proj\CodeCocks\Hively_Replayer\ahx2play.exe

del %out%
gcc -o %out% %files% %opts% %linkinc% 2> %errlog%
call :checkerr
exit /B 0



:checkerr
IF %ERRORLEVEL% NEQ 0 (
    echo oops!
    notepad %errlog%
    goto :end
)
for %%R in (%errlog%) do if %%~zR lss 1 del %errlog%
:end
exit /B 0