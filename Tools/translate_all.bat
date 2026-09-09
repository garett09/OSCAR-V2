@echo off
setlocal

set SCRIPT=C:\OSCAR\OSCAR-code\Tools\translate_ts_properly.py

for %%L in (
    Afrikaans.af
    Arabic.ar
    Bulgarian.bg
    Chinese.zh_CN
    Chinese.zh_TW
    Czech.cz
    Dansk.da
    Deutsch.de
    Espaniol.es
    Espaniol.es_MX
    Filipino.fil
    Francais.fr
    Greek.el
    Hebrew.he
    Italiano.it
    Japanese.ja
    Korean.ko
    Magyar.hu
    Nederlands.nl
    Norsk.no
    Polski.pl
    Portugues.pt
    Portugues.pt_BR
    Romanian.ro
    Russkiy.ru
    Suomi.fi
    Svenska.sv
    Thai.th
    Turkish.tr
    Ukrainska.uk
) do (
    echo === %%L ===
    python "%SCRIPT%" %%L
    if errorlevel 1 echo ERROR on %%L
    echo.
)

echo All done.
endlocal
