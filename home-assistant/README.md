# Home Assistant setup

The ESP32 shows up in Home Assistant through the ESPHome integration once it's
on your network (Settings → Devices & services → ESPHome, then enter the API
encryption key from your `secrets.yaml`). That gives you the desk entities:

| Entity | What it does |
|---|---|
| Sit / Stand (buttons) | Drive to the Sit Height / Stand Height and stop there |
| Stop (button) | Let go of every key; the desk stops immediately |
| Nudge Up / Nudge Down (buttons) | Hold Up or Down for one second |
| Height (sensor) | Height in inches, read from the handset display data |
| Position (sensor) | `sitting` or `standing`, split halfway between the two heights |
| Sitting Minutes (sensor) | Minutes since the desk last went to sitting height |
| Sit Height / Stand Height / Stop Early By (numbers) | Settings, under the device's Configuration section |
| Control box connected, Bus raw | Diagnostics |

Your entity IDs depend on the device name and area, e.g. `button.smart_desk_stand`
or `button.office_smart_desk_stand`. Check them under Settings → Entities.

## Auto-stand

The automation stands the desk up after you've been sitting at it for a set time,
waits a set time, then lowers it again only if you're still there and it's still
up. It needs a presence sensor that turns `on` while you're at the desk (I use a
Zigbee mmWave sensor; anything binary works).

There are two ways to add it. Both create the same things.

### Option A: package file

Copy [`desk_autostand.yaml`](desk_autostand.yaml) to `config/packages/`, enable
packages in `configuration.yaml`, fix the entity IDs at the top of the file and
restart Home Assistant.

### Option B: UI only (no YAML files)

1. **Helpers** (Settings → Devices & services → Helpers → Create helper):
   - Toggle: `Desk auto stand` → `input_boolean.desk_auto_stand`
   - Number: `Stand after sitting`, 5–120, step 5, unit `min` → `input_number.desk_sit_limit`
   - Number: `Stand for`, 5–60, step 5, unit `min` → `input_number.desk_stand_duration`
   - Template → Binary sensor: `Sitting at desk`, state template:
     ```
     {{ is_state('binary_sensor.mmwave_sensor_occupancy', 'on')
        and is_state('sensor.smart_desk_position', 'sitting') }}
     ```
2. **Automation** (Settings → Automations → Create → Edit in YAML): paste the
   `automation:` entry from `desk_autostand.yaml` (everything from `alias:` down,
   without the leading `- id:` line), with your entity IDs.
3. Turn on `Desk auto stand`.

### Why the once-a-minute trigger

A state trigger with `for:` only counts from the moment the state changes. If
Home Assistant restarts while you're sitting, "Sitting at desk" never changes and
the first trigger never fires. The `time_pattern` trigger plus the template
condition checks `last_changed` every minute, so it still fires after a restart.
