/*
 * DOS-XC /os/shell/logo.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#define LOGO_WIDTH 26
#define LOGO_LINES 14
static const char *const logo[LOGO_LINES] = {
    "\033[0;30m\xdb\033[0;30;41m\xdf\xdf\xdf\xdf\xdf\xdf\xdf\033[0;30m\xdb\033[0;30;44m\xdf\033[0;30;45m\xdf\033[0;94;40m\xdc\033[0;30;45m\xdf\xdf\xdf\033[0;30m\xdb\033[0;30;43m\xdf\033[0;93;40m\xdc\xdc\xdc\xdc\xdc\033[0;30;43m\xdf\033[0;90m\xdc\033[0m\033[0m",
    "\033[0;30m\xdb\033[0;31m\xdb\xdb\xdb\xdb\xdb\xdb\033[0;30m\xdb\033[0;34;45m\xdf\033[0;94;45m\xdc\033[0;35m\xdb\033[0;35;44m\xdf\033[0;35m\xdb\033[0;35;44m\xdf\033[0;90;40m\xdc\033[0;93;43m\xdc\033[0;93m\xdb\033[0;93;43m\xdf\xdf\xdf\xdf\xdf\033[0;93m\xdb\xdb\033[0;93;43m\xdc\033[0;30m\xdb\033[0m",
    "\033[0;30m\xdb\033[0;31m\xdb\xdb\xdb\033[0;31;40m\xdf\xdf\033[0;30m\xdb\033[0;94;45m\xdc\xdf\033[0;34;40m\xdf\033[0;31m\xdb\xdb\033[0;30m\xdb\033[0;34;40m\xdf\033[0;90;40m\xdf\033[0;93m\xdb\033[0;33m\xdb\033[0;90;40m\xdc\033[0;94;45m\xdc\033[0;34;45m\xdf\033[0;30m\xdb\xdf\033[0;33;40m\xdf\xdf\xdf\033[0;30m\xdf\033[0m",
    "\033[0;30m\xdb\033[0;31m\xdb\xdb\xdb\033[0m  \033[0;30m\xdb\033[0;94;45m\xdc\033[0;35m\xdb\033[0;30m\xdb\033[0;31m\xdb\xdb\033[0;30m\xdb\033[0m \033[0;30m\xdf\033[0;33;40m\xdf\xdf\033[0;90;40m\xdc\033[0;94;45m\xdc\xdf\033[0;30m\xdb\033[0;93;40m\xdc\033[0;30;43m\xdf\033[0;30m\xdc\033[0m\033[0m",
    "\033[0;30m\xdb\033[0;31m\xdb\xdb\xdb\033[0m  \033[0;30m\xdb\033[0;94;45m\xdc\033[0;35m\xdb\033[0;30m\xdb\033[0;31m\xdb\xdb\033[0;30m\xdb\033[0m   \033[0;30m\xdb\033[0;90;40m\xdc\033[0;35m\xdb\033[0;94;45m\xdf\033[0;30m\xdb\033[0;93;43m\xdf\033[0;93m\xdb\xdb\033[0;93;43m\xdc\033[0;30m\xdc\033[0m",
    "\033[0;30m\xdb\033[0;31m\xdb\xdb\xdb\033[0;30m\xdc\xdc\xdc\033[0;35;40m\xdf\033[0;34;41m\xdf\033[0;30;41m\xdf\033[0;31m\xdb\033[0;31;40m\xdf\033[0;30m\xdb\xdc\xdb\033[0;93;43m\xdc\xdc\033[0;93;40m\xdc\033[0;94;40m\xdf\033[0;34;40m\xdf\033[0;30m\xdc\033[0;90;40m\xdc\033[0;93;43m\xdc\033[0;93m\xdb\xdb\033[0;30m\xdb\033[0m",
    "\033[0;30m\xdb\033[0;31m\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\033[0;31;40m\xdf\033[0;30;44m\xdf\033[0;34;45m\xdf\033[0;94;45m\xdc\033[0;30m\xdb\033[0;33m\xdb\033[0;93;43m\xdf\033[0;93m\xdb\xdb\033[0;93;43m\xdc\xdc\033[0;93m\xdb\xdb\033[0;93;43m\xdf\033[0;33m\xdb\033[0;30m\xdb\033[0m",
    "\033[0;30m\xdb\033[0;31m\xdb\xdb\xdb\xdb\xdb\xdb\xdb\xdb\033[0;30m\xdb\033[0;90;44m\xdc\033[0;35;44m\xdf\xdf\xdf\033[0;34;45m\xdf\033[0;30;44m\xdf\033[0;33;40m\xdf\033[0;33m\xdb\033[0;93;43m\xdf\xdf\xdf\xdf\033[0;33m\xdb\xdb\033[0;33;40m\xdf\033[0;30m\xdf\033[0m",
    "\033[0;30m\xdf\xdb\xdb\033[0;90;40m\xdc\xdc\033[0;30m\xdb\xdb\xdf\xdb\033[0;90;40m\xdc\xdc\xdc\033[0;30m\xdb\033[0;90m\xdf\033[0;30m\xdb\033[0;90;40m\xdf\xdc\xdc\033[0;90m\xdb\033[0;90;40m\xdc\xdf\xdf\033[0;30m\xdf\xdf\033[0m\033[0m",
    "\033[0m  \033[0;90;40m\xdf\033[0;97;40m\xdf\033[0;37m\xdb\033[0;90;40m\xdc\033[0;30m\xdc\033[0m \033[0;90m\xdc\033[0;90;47m\xdf\033[0;37m\xdc\033[0;90;47m\xdc\033[0;30m\xdf\033[0m \033[0;90m\xdb\033[0;37m\xdb\033[0;90;47m\xdc\033[0;90m\xdf\xdf\xdf\033[0;90;47m\xdc\xdf\033[0;90m\xdb\033[0m\033[0m",
    "\033[0m   \033[0;30m\xdf\033[0;37;40m\xdf\033[0;90;47m\xdc\xdf\xdf\033[0;37m\xdb\033[0;90;47m\xdc\033[0;90m\xdf\033[0m   \033[0;90m\xdb\033[0;37m\xdb\033[0;90m\xdb\033[0m   \033[0;30m\xdf\xdf\xdf\033[0m\033[0m",
    "\033[0m    \033[0;90m\xdc\033[0;90;47m\xdf\033[0;37m\xdb\xdb\033[0;90;47m\xdf\033[0;90;40m\xdc\033[0;30m\xdc\033[0m   \033[0;90m\xdb\033[0;37m\xdb\033[0;90m\xdb\033[0m\033[0m",
    "\033[0m  \033[0;90m\xdc\033[0;90;47m\xdf\033[0;37m\xdb\033[0;90;47m\xdc\033[0;90m\xdf\033[0;30m\xdf\033[0;37;40m\xdf\033[0;37m\xdb\033[0;90;47m\xdf\033[0;90;40m\xdc\033[0;30m\xdc\033[0m \033[0;90m\xdb\033[0;37m\xdb\033[0;90m\xdb\033[0;30m\xdc\xdc\xdc\033[0;30;47m\xdf\033[0;90;47m\xdf\033[0;90;40m\xdc\033[0m\033[0m",
    "\033[0m  \033[0;90;40m\xdf\033[0;37;40m\xdf\xdf\033[0;30m\xdb\033[0m   \033[0;90;40m\xdf\033[0;37;40m\xdf\xdf\033[0;30m\xdb\033[0m  \033[0;90m\xdf\033[0;90;40m\xdf\033[0;37;40m\xdf\xdf\xdf\033[0;90;40m\xdf\033[0;30m\xdf\xdf\033[0m\033[0m",
};
