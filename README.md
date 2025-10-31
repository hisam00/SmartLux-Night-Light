# SmartLux Night Light

SmartLux Night Light is an IoT-based smart night light system designed to monitor environmental conditions and provide intelligent lighting control. The project integrates sensor data collection, cloud connectivity (Firebase), and a web dashboard for real-time monitoring and management.

## Features

- **Automatic Light Control**: Turns the night light on or off based on ambient light and motion detection.
- **Sensor Monitoring**: Collects and uploads data on light intensity (LDR), motion, humidity, and temperature.
- **Cloud Integration**: Uses Firebase Realtime Database for device communication and data storage.
- **Realtime Web Dashboard**: Visualizes sensor data and allows remote control of the night light via a web interface.
- **Notification System**: Configurable notifications/webhooks for detected motion or temperature thresholds.

## Hardware Components

- Microcontroller (e.g., ESP32 or ESP8266)
- LDR (Light Dependent Resistor) sensor
- PIR Motion sensor
- DHT sensor (for humidity and temperature)
- LED or relay for controlling the light

## How It Works

### Device Firmware (Arduino/C++)

1. **Sensor Data Collection**: The microcontroller reads data from the LDR, motion, humidity, and temperature sensors.
2. **Data Upload**: Periodically, the device sends sensor data to Firebase in JSON format:
    - LDR value
    - Motion detected (`true`/`false`)
    - Humidity
    - Temperature
3. **Remote Commands**: The device listens for control commands (e.g., turn LED on/off) from Firebase, allowing remote override from the web dashboard.
4. **Notification Config Fetching**: The device periodically fetches notification settings (e.g., webhook URLs, reboot/light control) from Firebase.
5. **Event Notifications**: On motion or high temperature, the device can trigger webhooks or other notification mechanisms.

### Web Dashboard

- **Dashboard**: Real-time charts and tables showing historical and current sensor data.
- **Remote Control**: Send commands to override the night light state.
- **User Authentication**: Uses Firebase Auth for secured access.
- **Notification Configuration**: Set up notification thresholds and webhook URLs.

## Project Structure

```
IOT/
  smartluxnighlight/
    smartluxnighlight.ino   # Main device firmware
WEBSITE/
  admin-backend/           # Node.js backend for admin operations
    server.js
  public/                  # Web dashboard static files
    index.html
    dashboardadmin.html
    firebase-config.js
    ...
```
## How to Get Started - Easy (Website)

1. Access Our [Website](https://smartlux-night-light.web.app/) through the link below:
   - Website Link: https://smartlux-night-light.web.app/
       
## How to Get Started - Advance (Website)

1. **Install Mini Anaconda**

   Make sure you have [Mini Anaconda](https://www.anaconda.com/download) installed on your system.

2. **Install Node.js**

   Make sure you have Anaconda Prompt open
   
   From there, type:
   ```sh
   conda install nodejs
   ```
2. **Install http-server**

   Make sure you have already installed NodeJS in Anaconda
   
   From Anaconda Prompt, type:
   ```sh
   npm install http-server
   ```

4. **Start the Admin Backend**
   ```sh
   cd SYSTEM FOLDER/WEBSITE/public/admin-backend
   npm install
   node server.js
   ```

5. **Serve the Public Dashboard**
   In a new terminal:
   ```sh
   cd SYSTEM FOLDER/WEBSITE/public
   npx http-server
   ```
   > If `http-server` is not installed, you can install it globally with `npm install -g http-server`.

6. **Access the Dashboard**
   Open your browser and go to the local server URL (usually http://localhost:8080).

## How to Get Started - Advance (Device)

1. Assemble the sensors and microcontroller according to your chosen schematic.
2. Flash the `smartluxnighlight.ino` firmware to your device.
3. Set up WiFi credentials in the firmware.

## User Credentials

**Admin**

- email: hisam@gmail.com  
  Password: 123456

- Username: azim@gmail.com  
  Password: 123456

- Username: ryoga@gmail.com  
  Password: 123456

&nbsp;
**User**

- Username: irfan@gmail.com  
  Password: 123456

- Username: izzah@gmail.com  
  Password: 123456


## Acknowledgments

- Inspired by smart home and IoT automation projects.
- Built using [Firebase](https://firebase.google.com/) and Arduino-compatible microcontrollers.

### Project Team G6

- [hisam00](https://github.com/hisam00)
- [azim](https://github.com/azimkhan05)
- [ryoga](https://github.com/RyogaF22)
- [irfan](https://github.com/RAKABA379)
- [izzah](https://github.com/ZaxleX1)

### Special Thanks

- [Miss_Syira](https://github.com/syiraazhari) for guidance and support.
