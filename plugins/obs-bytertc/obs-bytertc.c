#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-outputs", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "OBS ByteDance RTC Streaming";
}

extern struct obs_output_info bytertc_output_info;

bool obs_module_load(void)
{
	obs_register_output(&bytertc_output_info);
	return true;
}

void obs_module_unload(void)
{
}
