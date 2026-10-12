import SafariServices
import AppKit
import os.log

/// Safari uzantısının yerel mesaj işleyicisi (HB Download Manager).
/// Tek görevi: popup'tan gelen "launchApp" isteğiyle ana uygulamayı başlatmak.
final class SafariWebExtensionHandler: NSObject, NSExtensionRequestHandling {

    func beginRequest(with context: NSExtensionContext) {
        let item = context.inputItems.first as? NSExtensionItem
        let message = item?.userInfo?[SFExtensionMessageKey] as? [String: Any]
        var reply: [String: Any] = ["ok": false]

        if (message?["action"] as? String) == "launchApp" {
            // .appex → PlugIns → Contents → HB Download Manager.app
            let appURL = Bundle.main.bundleURL
                .deletingLastPathComponent()   // PlugIns
                .deletingLastPathComponent()   // Contents
                .deletingLastPathComponent()   // .app
            if appURL.pathExtension == "app" {
                let cfg = NSWorkspace.OpenConfiguration()
                cfg.activates = false
                NSWorkspace.shared.openApplication(at: appURL, configuration: cfg) { _, _ in }
                reply = ["ok": true]
            }
        }

        let response = NSExtensionItem()
        response.userInfo = [SFExtensionMessageKey: reply]
        context.completeRequest(returningItems: [response], completionHandler: nil)
    }
}
