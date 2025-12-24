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
1. Install prerequisites via winget (or download manually):
   ```powershell
   winget install Kitware.CMake Git.Git Microsoft.VisualStudio.2022.BuildTools
   ```
   - Include the "Desktop development with C++" workload in Visual Studio Build Tools.
2. Install vcpkg and integrate (if not already):
   ```powershell
   git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg integrate install
   ```
3. Install Drogon and dependencies:
   ```powershell
   C:\vcpkg\vcpkg install drogon jsoncpp openssl zlib
   ```
4. Configure and build the backend:
   ```powershell
   cmake -S backend -B backend\build -DDROGON_FOUND=ON -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
   cmake --build backend\build --config Release
   backend\build\Release\college_ff_server.exe
   ```
5. Serve the frontend (one option using Python):
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
