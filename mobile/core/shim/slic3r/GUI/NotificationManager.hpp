// Stand-in for slic3r/GUI/NotificationManager.hpp: the notifications the plugin system pushes, shown by the app
// (Plugins.cpp: a "notification" event; its hyperlink calls the callback back)
#pragma once
#include <functional>
#include <string>

class wxEvtHandler;

namespace Slic3r { namespace GUI {

enum class NotificationType { CustomNotification = 0, OrcaCloudAPIError };

class NotificationManager
{
public:
    enum class NotificationLevel : int {
        ProgressBarNotificationLevel = 1,
        HintNotificationLevel,
        RegularNotificationLevel,
        PrintInfoNotificationLevel,
        PrintInfoShortNotificationLevel,
        ImportantNotificationLevel,
        WarningNotificationLevel,
        SeriousWarningNotificationLevel,
        ErrorNotificationLevel,
    };
    void push_notification(NotificationType type, NotificationLevel level, const std::string &text, const std::string &hypertext = "",
                           std::function<bool(wxEvtHandler *)> callback = std::function<bool(wxEvtHandler *)>(), int timestamp = 0);
};

}} // namespace Slic3r::GUI
