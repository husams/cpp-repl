#ifdef DEMO_VALUE
#error Per-file database flags leaked into the session defaults
#endif
int no_defaults_value() { return 17; }
