# SharkShit64 Firmware (for ESP32 based clone)



This is the firmware used to work together with Richard Weick's SharkShit64 open source ESP32 based Interact Sharkwire Online clone.



(link here when available)


### Mission
The mission of this project is to show people who complain about things and wish they could "go back to the 90s" what it was REALLY like, and how painful the internet was back then.

Just kidding :p

This project (hopefully) intends to faithfully re-create the exerience of Interact's Sharkwire Online as it was during its very short lived release back in 1999, with potentially a few added features.

This is all currently a WIP, but does include some basic functionality.

What works:
* Sharkwire Online activation (pretty much as originally intended)
* Creating/Deleting users, as well as all original settings
* The internet over a WiFi connection (HTTP only @ 19200bps - for now)
* BLE Keyboards
* ESP32 AP based configuration tool
* Custom Sharkwire Online home page

What doesn't work:
* HTTPS
* E-Mail (yet)

Added software features:
The ability go GOTO any (HTTP) website directly from the UI (a feature AFAIK never enabled on retail)

### Usage
You'll likely want to just obtain the SharkShit64 open source ESP32 based Interact Sharkwire Online clone cart, but one could potentially build one out of an Interact GameShark, but it requires some very specific components and probably not worth all of the work needed to achieve this.

Once you have the cart...

Step 1: You will want to flash the release file(s) to your SharkShit64 cart (link to GUI flash tool here when available)

Step 2: You will then want to set up your WiFi credentials by getting on a WiFi capable device (PC, or phone, for example) and connecting to the SSID "SharkShit64" and then in your web browser on the connected device you'll want to go to http://192.168.4.1 at which point you should be greeted with an SSID and Password field that you will then use to fill in your internet connected router WiFi credentials.
Then you will select "Save" and will be asked to power cycle the Nintendo64.

Step 3: Power cycle the Nintendo64...

Step 4: Upon power up, you will want to reconnect to the "SharkShit64" SSID and go to http://192.168.4.1 again, and then grab a BLE enabled keyboard and put it into SYNC mode (the earlier during boot you put the keyboard into SYNC mode, the better.) 

The configuration UI should eventually show that a device has connected (if it's supported) and you'll be greeted with a message telling you to once again, power cycle the Nintendo64...

Step 5: Power cycle the Nintendo64... I know, hard to believe.

Step 6: After this is done, upon boot, your WiFi and BLE device should be set persistently, and will not have to be reconfigured again unless you device to do so.
You should now boot into an activation page. You can use any credentials to sign into this page, and once connected, you should get a screen telling you that you have completed activation and to power cycle.

Step 7: Shockingly, power cycle again...

Step 8: When the system boots this time, you'll then be able to add a new user by selecting one of the "Add new user" buttons.
You'll then be asked to select a username and password, and once you do that and select "Add to account" you'll then be asked to power cycle... again...

Step 8: PoWeR CyCle tHe NinTenDo64 Plz

Step 9:
This should be the last time for power cycling unless you change any settings, but upon rebooting this time, you'll then see options for email, go online, and a few other options to change Sharkwire Online settings.
At this point you can test going online by selecting the option from the menu.

### Building

(Will be posted at a later time)



### Credits

[@Modman](https://github.com/RWeick/)

[@Jhynjhiruu](https://github.com/Jhynjhiruu/)

[@ppcasm](https://github.com/ppcasm/)

