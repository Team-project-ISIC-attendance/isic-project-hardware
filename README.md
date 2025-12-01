# isic-project-hardware


```
Shortcut for defines based on hardware target and build type

#ifdef HW_TARGET_ESP32
// ESP32-specific FreeRTOS tuning or dual-core pinning
#elif defined(HW_TARGET_ESP8266)
// Lighter tasks, smaller stacks, etc.
#endif

#ifdef PROD_BUILD
// No debug APIs, no test commands, reduced logging
#else
// Extra diagnostics / test points
#endif
```