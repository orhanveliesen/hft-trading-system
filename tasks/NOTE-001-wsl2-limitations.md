# NOTE-001: WSL2 Environment Limitations

## Priority: INFO

## Description
Documented limitations encountered when running the trading system in WSL2 environment.

## Limitations Found

### 1. Perf Profiler Not Available
- **Issue**: `--profile` flag fails - WSL2 lacks kernel tools for `perf stat`
- **Error**: Missing perf events, kernel modules
- **Workaround**: Use native Linux for profiling, or use application-level timing
- **Impact**: Cannot collect CPU profiling data in WSL2

### 2. Dashboard High CPU Usage (641-762%)
- **Issue**: ImGui render loop spins without proper vsync in WSL2/X11
- **Cause**: X11 forwarding + OpenGL rendering inefficiency
- **Workaround**:
  - Run dashboard on Windows native
  - Or accept high CPU usage
  - Or add manual frame limiting in dashboard code
- **Impact**: High system resource usage, but functional

## Recommendations
- For development/testing: WSL2 is acceptable
- For profiling: Use native Linux VM or bare metal
- For production: Use native Linux

## Found
- Date: 2026-02-18
- Session: Paper trading 2h session with --profile flag
