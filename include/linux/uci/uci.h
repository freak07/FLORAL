#ifndef __UCI_H__
#define __UCI_H__

#define UCI_INVALID_INT -999999

// user config file to read data coming from user space
#define UCI_USER_FILE "/storage/emulated/0/uci_user.cfg"
// sys file to read from user space
#define UCI_SYS_FILE "/storage/emulated/0/uci_sys.cfg"
// file to write data from kernel side to unelevated access
#define UCI_KERNEL_FILE "/storage/emulated/0/uci_kernel.out"

#define UCI_USER_FILE_END "uci_user.cfg"
#define UCI_SYS_FILE_END "uci_sys.cfg"
#define UCI_KERNEL_FILE_END "uci_kernel.out"

#define UCI_HOSTS_FILE "/storage/emulated/0/hosts_k"
#define UCI_HOSTS_FILE_END "hosts_k"

// pstore files to grant access to, without superuser elevation
#define UCI_PSTORE_FILE_0 "/sys/fs/pstore/console-ramoops"
#define UCI_PSTORE_FILE_1 "/sys/fs/pstore/console-ramoops-0"

#define UCI_PSTORE_FILE_0_END "console-ramoops"
#define UCI_PSTORE_FILE_1_END "console-ramoops-0"

extern bool is_uci_path(const char *file_name);
extern bool is_uci_file(const char *file_name);

extern void notify_uci_file_closed(const char *file_name);
extern void notify_uci_file_write_opened(const char *file_name);

/** accesing kernel settings from UCI property configuration
*/
extern int uci_get_user_property_int(const char* property, int default_value);
extern int uci_get_user_property_int_mm(const char* property, int default_value, int min, int max);
/** accesing kernel settings from UCI property configuration
*/
extern const char* uci_get_user_property_str(const char* property, const char* default_value);

/** accesing sys variables from UCI sys props
*/
extern int uci_get_sys_property_int(const char* property, int default_value);
extern int uci_get_sys_property_int_mm(const char* property, int default_value, int min, int max);
/** accesing sys variables from UCI sys props
*/
extern const char* uci_get_sys_property_str(const char* property, const char* default_value);

/** add change listener to sys cfg*/
extern void uci_add_sys_listener(void (*f)(void));
/** add change listener to user cfg*/
extern void uci_add_user_listener(void (*f)(void));

/** write operations */
extern void write_uci_out(char *message);

#endif /* __UCI_H__ */
