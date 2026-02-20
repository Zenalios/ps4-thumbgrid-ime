# Contributing to PS4 ThumbGrid IME

Thanks for your interest in contributing! Here's how you can help.

## Reporting Issues

- Use [GitHub Issues](https://github.com/Zenalios/ps4-thumbgrid-ime/issues) to report bugs or suggest features
- Include your PS4 firmware version and GoldHEN version
- For bugs, describe what you expected vs. what happened
- KLog output (`nc <PS4_IP> 3232`) is very helpful for debugging

## Development Setup

### Prerequisites

- Linux build host (tested on WSL2)
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- [GoldHEN Plugins SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK)
- clang, lld, llvm

### Building

```bash
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
export GOLDHEN_SDK=/path/to/GoldHEN_Plugins_SDK

# Game-side plugin
cd custom-ime
make clean && make

# Shell overlay
cd shell-overlay
make clean && make
```

### Deploying for Testing

```bash
curl -T bin/thumbgrid_ime.prx ftp://<PS4_IP>:2121/data/GoldHEN/plugins/
curl -T shell-overlay/bin/shell_overlay.prx ftp://<PS4_IP>:2121/user/data/
```

Reboot your PS4 after deploying to load the new PRX files.

## Pull Requests

1. Fork the repository
2. Create a feature branch (`git checkout -b my-feature`)
3. Make your changes
4. Test on a real PS4 if possible
5. Commit with a clear message describing the change
6. Push and open a Pull Request

### Guidelines

- Keep changes focused — one feature or fix per PR
- Follow the existing code style (C99, 4-space indent)
- Test both the game-side plugin and shell overlay if your change affects IPC
- Update the README if you add new features or change controls

## Project Structure

| File | Description |
|------|-------------|
| `src/ime_hook.c` | IME dialog hooks, controller input, main logic |
| `src/ime_custom.c` | Text session state (cursor, selection, clipboard) |
| `src/thumbgrid.c` | 3x3 grid engine (pages, cell layout, accents) |
| `src/input.c` | Controller input edge detection |
| `include/thumbgrid_ipc.h` | Shared IPC struct definition |
| `shell-overlay/src/main.c` | PUI overlay (Mono runtime, widgets, IPC reader) |

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
