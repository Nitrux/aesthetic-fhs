/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AFHS_H
#define AFHS_H

#define AFHS_GENL_NAME "aesthetic_fhs"
#define AFHS_GENL_VERSION 1

enum afhs_command {
	AFHS_CMD_UNSPEC,
	AFHS_CMD_HIDE,
	AFHS_CMD_UNHIDE,
	AFHS_CMD_FLUSH,
	AFHS_CMD_LIST,
};

enum afhs_attribute {
	AFHS_ATTR_UNSPEC,
	AFHS_ATTR_PATH,
};

#endif
