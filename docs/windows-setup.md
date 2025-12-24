# Windows 11 Setup (C++ Drogon + Frontend)

This guide outlines how to build the backend and serve the frontend on Windows 11. Two options are provided: native Windows with vcpkg, or Windows Subsystem for Linux (WSL).

## Recommended: WSL
1. Install WSL (Ubuntu) from the Microsoft Store.
2. Inside WSL, run the Debian/Ubuntu helper script:
   ```bash
   ./scripts/install_dependencies.sh
   ```
3. Build the backend:
   ```bash
   cmake -S backend -B backend/build -DDROGON_FOUND=ON
   cmake --build backend/build
   ./backend/build/college_ff_server
   ```
4. Serve the frontend (e.g., from `frontend/`):
   ```bash
   python -m http.server 3000
   ```

## Native Windows (vcpkg)
1. Option A: run the automated installer (PowerShell, may prompt for elevation):
   ```powershell
   ./scripts/install_windows.ps1
   ```
   - Default triplet: `x64-windows`. Use `-Triplet x64-windows-static` to change.
   - vcpkg location defaults to `%USERPROFILE%\vcpkg`; override with `-VcpkgDir "C:\\path\\to\\vcpkg"`.
2. Option B: manual steps
   ```powershell
   winget install Microsoft.VisualStudio.2022.BuildTools --accept-package-agreements --accept-source-agreements
   winget install Kitware.CMake --accept-package-agreements --accept-source-agreements
   winget install Git.Git --accept-package-agreements --accept-source-agreements
   git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg install drogon jsoncpp openssl zlib libuuid
   C:\vcpkg\vcpkg integrate install
   ```
3. Configure and build the backend (vcpkg toolchain):
   ```powershell
   cmake -S backend -B backend\build -DDROGON_FOUND=ON -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -A x64
   cmake --build backend\build --config Release
   backend\build\Release\college_ff_server.exe
   ```
4. Serve the frontend (one option using Python):
   ```powershell
   cd frontend
   python -m http.server 3000
   ```

## Docker Desktop
If you prefer containers, install Docker Desktop for Windows and run:
```powershell
docker compose up --build
```
The backend will be on `http://localhost:8080` and the frontend on `http://localhost:3000`.

## Environment variables
- `JWT_SECRET` (required for secure endpoints)
- `PORT` (backend port, default `8080`)
- `ALLOWED_ORIGINS` (comma-separated CORS allowlist)
- `SSL_CERT_FILE` / `SSL_KEY_FILE` (optional HTTPS)
