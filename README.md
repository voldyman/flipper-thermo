# Thermometer

This flipper zero app reads temperature and humidity from a AM2301A sensor and displays it on the screen.

## Usage

Use the [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) tool to build and use the app.

```shell
ufbt launch
```

or just `ufbt` to build the app.
I use `ufbt cli` and then `logs trace` to read debug logs from the app.
Use `ufbt vscode_dist` to setup vscode paths for development.

![screenshot](https://github.com/voldyman/flipper-thermo/raw/de0448e33e58639f489f32131c5978923289fb50/screenshot.png)