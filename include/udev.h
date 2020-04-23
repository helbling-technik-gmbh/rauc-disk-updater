#ifndef __RAUC_USB_UPDATER__UDEV_H__
#define __RAUC_USB_UPDATER__UDEV_H__


#include <glib.h>
#include <glib-object.h>
#include <gudev/gudev.h>

G_BEGIN_DECLS


#define UDEV_TYPE_MONITOR udev_monitor_get_type ()
G_DECLARE_FINAL_TYPE (UdevMonitor, udev_monitor, UDEV, MONITOR, GObject)


UdevMonitor *udev_monitor_new (void);
void udev_monitor_quit(UdevMonitor *provider);

G_END_DECLS	

#endif // __RAUC_USB_UPDATER__UDEV_H__
