# brain_zero

brain_zero is a Wi-Fi dongle for Victron MultiPlus-II inverter/chargers. It plugs into the inverter’s VE.Bus port and gives you a local dashboard. On supported firmware, it can also control ESS and read or write VEConfigure settings.

<img src="images/brain-zero-on-inverter.webp" alt="brain_zero in its black enclosure on top of a blue Victron inverter, connected by a yellow VE.Bus cable" width="500">

## Software

The screenshots show the local web interface with example data. Select a screenshot to open it full size.

**Dashboard** · Battery state of charge and live AC and DC measurements.

[![brain_zero dashboard with battery and inverter gauges](images/dashboard.png)](images/dashboard.png)

| ESS settings | Inverter controls |
| --- | --- |
| Grid setpoint, charge and discharge limits, and scheduled charge levels. | Operating mode, AC input current limit, and feedback from VE.Bus. |
| [![ESS settings with scheduled charge levels](images/ess.png)](images/ess.png) | [![Inverter status and controls](images/inverter.png)](images/inverter.png) |

| Multi settings | Diagnostics |
| --- | --- |
| Read the inverter setup and download or upload a VEConfigure settings file. | See live measurements, ESS decisions, and connection health. |
| [![Multi settings with VEConfigure download and upload](images/multi-settings.png)](images/multi-settings.png) | [![Diagnostics showing readings and ESS decisions](images/diagnostics.png)](images/diagnostics.png) |

## MultiPlus-II compatibility

These profiles are for standalone 230 V units. Check the full seven-digit VE.Bus firmware number in Diagnostics or Victron’s tools.

| Model | Firmware | Test coverage |
| --- | --- | --- |
| MultiPlus-II 48/5000/70-50 | `2623563` | ESS verified on physical hardware. VEConfigure settings writes supported. |
| MultiPlus-II 48/5000/70-50 | `2623556` | ESS tested in replay. |
| MultiPlus-II 48/3000/35-32 | `2629556` | ESS and VEConfigure settings writes tested in simulation. |
| MultiPlus-II 24/3000/70-32 | `2611552` | ESS and VEConfigure settings writes tested in simulation. |
| MultiPlus-II 48/4k5/55-32 | `2601560` | ESS and VEConfigure settings writes tested in simulation. |

### Firmware we’d like to support

We’re collecting settings profiles for the revisions below. All ten passed monitoring checks in the Venus Pi simulator. ESS and settings writes need further testing with matching exports. If your model or firmware is missing, [send us a compatibility profile](#share-a-compatibility-profile).

| Model | Firmware wanted |
| --- | --- |
| **MultiPlus-II 24/3000/70-32** | **`2611556`** |
| **MultiPlus-II 48/4k5/55-32** | **`2601570`** |
| MultiPlus-II 48/5000/70-50 | `2623570` |
| MultiPlus-II 48/6k5/100-50 | `2602570` |
| MultiPlus-II 48/3000/35-32 | `2629570` |
| MultiPlus-II 48/8000/110-100 | `2621570` |
| MultiPlus-II 12/3000/120-32 | `2605558` |
| MultiPlus-II 48/10000/140-100 | `2627570` |
| MultiPlus-II 24/5000/120-50 | `2615559` |
| MultiPlus-II 48/15000/200-100 | `2619570` |

## Hardware

![3D rendering of the populated brain_zero Rev C board](images/brain-zero-rev-c.png)

[View the board, enclosure, and bundle at brain-zero.com](https://brain-zero.com/). Checkout is currently a preview.

For a DIY build, the [esp32ess hardware guide](https://github.com/pv-baxi/esp32ess/blob/main/docs/README.md) shows how to connect an ESP32 to a single MultiPlus-II over VE.Bus. It’s a separate project with its own firmware.

## Share a compatibility profile

The current brain_zero firmware can download the settings backup we need directly from your inverter. Once we’ve verified ESS control and VEConfigure settings writes for your exact model and firmware, we’ll add that combination to the fully supported list.

1. With brain_zero connected to your inverter, open **System → VEConfigure** and select **Download settings**. This saves a fresh `.rvsc` backup without changing your inverter settings. If the download is unavailable, you can use [Victron Remote VEConfigure](https://www.victronenergy.com/media/pg/VRM_Portal_manual/en/remote-veconfigure.html) to get the backup instead.
2. Create the file you can share. Open the [compatibility exporter](tools/export_compatibility.py) on GitHub, select **Download raw file**, and run it locally with Python 3.10 or later. Replace the example path with the `.rvsc` you downloaded. On macOS or Linux, use `python3` in place of `py -3`.

   ```powershell
   py -3 export_compatibility.py "path\to\backup.rvsc" --output compatibility.json
   ```

3. Open `compatibility.json` in a text editor and paste its contents into the [compatibility form](https://github.com/mjpalmowski/brain_zero/issues/new?template=compatibility.yml). Include your inverter model and the full VE.Bus firmware number if you know it.

GitHub issues are public. Keep the original `.rvsc` on your computer and share only `compatibility.json`. The profile includes the firmware and settings limits we need for testing. It leaves out your serial number, unit ID, embedded credentials, saved settings and Assistant contents.

## License

[MIT License](LICENSE)
