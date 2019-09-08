# Build Board!

Displays your build status on an LED matrix.

https://twitter.com/praeclarum/status/1164251822720032769


## Make your own keys.h

To keep secrets secret, you must manually create the file **keys.h**.
The file should contain these lines:

```c
#define WIFI_SSID "wifi name"
#define WIFI_PASS "wifi password"

#define BITRISE_TOKEN "blahblahblahblahblahblah"
#define BITRISE_USER  "741a2e6502f76b0a"
```

and now fill in those string with good values.
