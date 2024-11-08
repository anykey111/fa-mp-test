
set WORKDIR=c:\ProgramData\FAForever\bin\
cd %WORKDIR%
echo %WORKDIR%

start /B ForgedAlliance.exe /init init.lua /gpgnet 127.0.0.1:7237 /log fa1.log /nobugreport
timeout 5
start /B ForgedAlliance.exe init init.lua /gpgnet 127.0.0.1:7237 /log fa2.log /nobugreport
timeout 1
echo start /B ForgedAlliance.exe /init init.lua /gpgnet 127.0.0.1:7237 /log fa3.log /nobugreport
echo timeout 1
