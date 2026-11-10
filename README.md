# brain_zero

brain_zero is a Wi-Fi dongle for Victron MultiPlus-II inverter/chargers. It reads VE.Bus data and supports ESS control and VEConfigure settings writes on tested hardware and firmware combinations.

If you want to use brain_zero with your inverter, [submit a compatibility profile](https://github.com/mjpalmowski/brain_zero/issues/new?template=compatibility.yml). We’re starting with standalone 230 V MultiPlus-II units. Profiles for a 24/3000 on `2611556` and a 48/4k5 on `2601570` are especially welcome.

1. In VRM, go to **Device list → Remote VEConfigure** and download the `.rvsc` backup for your inverter. [Victron’s download instructions](https://www.victronenergy.com/media/pg/VRM_Portal_manual/en/remote-veconfigure.html)
2. Open the [exporter](tools/export_compatibility.py) on GitHub, choose **Download raw file**, and run it on your computer with Python 3.10 or later. Use `python3` in place of `py -3` on macOS or Linux.

   ```powershell
   py -3 export_compatibility.py "path\to\backup.rvsc" --output compatibility.json
   ```

3. Open `compatibility.json` in a text editor. Paste its contents into the compatibility form and add your inverter model and firmware if you know them.

The exporter runs locally. It retains the firmware reply, settings limits and metadata needed to reproduce the unit in an isolated Venus OS simulator. It omits your serial number, unit ID, embedded credentials, saved settings and Assistant contents. GitHub issues are public. Keep the original backup on your computer and share only the generated profile.

Submitting a profile helps us reproduce your firmware in the simulator. Compatibility is tracked by hardware and firmware revision.
