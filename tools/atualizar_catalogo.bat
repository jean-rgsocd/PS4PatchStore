@echo off
rem Preenche data-content-id, data-size e data-type de TODOS os botoes do catalogo
rem lendo so o cabecalho de cada PKG (nao baixa o arquivo inteiro).
cd /d "%~dp0.."
python tools\pkg_info.py image0\index.html
if errorlevel 1 (
  echo.
  echo Falhou. Veja a mensagem ERRO acima ^(link fora do ar? PKG invalido?^).
  exit /b 1
)
echo.
echo Catalogo atualizado. Gere o app novamente.
