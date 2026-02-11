# my_ntpdate_command
An implementation of an original ntpdate command


## How to Run

You can build and run the program with the following steps:

```bash
$ make -f Make.my_ntpdate
$ ./my_ntpdate ntp.nict.jp
```

---

## Running with Custom Log Level and NTP Try Count

You can specify the log level and the number of NTP requests at build time.

In the example below, the log level is set to **INFO**, and the NTP request count is set to **1**.

> The default value of `NTP_TRY_COUNT` is **8**, which matches the default behavior of the `ntpdate` command.

```bash
$ make -f Make.my_ntpdate LOG_LEVEL=3 NTP_TRY_COUNT=1
$ ./my_ntpdate ntp.nict.jp
```

---

## Applying the Time to the System with `settimeofday`

By default, this program **does not modify the system clock**.  
It only calculates and prints the **delay** and **offset** based on the NTP server response.

If you want the program to apply the received time to the system using `settimeofday`, build it as follows:

```bash
$ make -f Make.my_ntpdate ENABLE_SETTIMEOFDAY=yes
$ ./my_ntpdate ntp.nict.jp
```

---

## Log Levels

The available log levels are:

```text
LOG_ERROR = 1
LOG_WARN  = 2
LOG_INFO  = 3
LOG_DEBUG = 4
```

---

## Notes and Cautions

- This program sends NTP requests to the address specified as the first argument using **UDP port 123**.
- If services such as `ntpd` or `chronyd` are already running on your system and using port 123, this program may not work correctly.
