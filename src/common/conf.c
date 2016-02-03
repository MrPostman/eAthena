// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder


#include "conf.h"
#include "../common/showmsg.h"

int config_load(config_t *config, const char *filename) {
	config_init(config);
	if (!config_read_file(config, filename)) {
		ShowError("%s:%d - %s\n", config_error_file(config),
			config_error_line(config), config_error_text(config));
		config_destroy(config);
		return 1;
	}

	return 0;
}