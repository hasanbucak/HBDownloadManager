#include "MediaHosts.h"
#include <QLatin1String>
#include <QLatin1Char>

bool MediaHosts::isStreamingSite(const QString &host) {
    static const char *const kDomains[] = {
        "youtube.com", "youtu.be", "vimeo.com", "dailymotion.com", "tiktok.com",
        "instagram.com", "facebook.com", "fb.watch", "fb.com", "twitch.tv", "x.com", "twitter.com"
    };
    const QString h = host.toLower();
    for (const char *d : kDomains) {
        const QString dom = QLatin1String(d);
        if (h == dom || h.endsWith(QLatin1Char('.') + dom)) {
            return true;
        }
    }
    return false;
}
