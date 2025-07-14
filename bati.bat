@echo off
echo ======================================
echo  LIMPIEZA DE HISTORIAL GIT PESADO
echo  (por Kevin y ChatGPT)
echo ======================================
echo.

REM Paso 1: Clonar el repositorio como mirror
git clone --mirror . F5-TTS-Espanol-clean.git

REM Paso 2: Ejecutar BFG para borrar archivos pesados
echo.
echo >> Ejecutando BFG para limpiar archivos *.pt, *.safetensors y carpeta venv...
java -jar bfg.jar --delete-files *.pt --delete-files *.safetensors --delete-folders venv F5-TTS-Espanol-clean.git

REM Paso 3: Limpiar referencias y empaquetar
cd F5-TTS-Espanol-clean.git
git reflog expire --expire=now --all
git gc --prune=now --aggressive

REM Paso 4: Forzar push al repositorio remoto
echo.
echo >> Enviando los cambios limpios a GitHub (esto puede tardar)...
git push --force

echo.
echo ✅ ¡Limpieza y push completados!
pause
