/**
 * @file src/QtTrayMenu.cpp
 * @brief Definitions for Qt tray menu implemenation
 */
// standard includes
#include <algorithm>
#include <filesystem>

// qt includes
#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QStyle>

// local includes
#include "QtTrayMenu.h"

namespace {
  int defaultArgc = 1;  // NOSONAR(cpp:S5421): This is required for QApplication's argc/argv constructor
  char defaultArgv0[] = "TrayMenuApp";  // NOSONAR(cpp:S5421): This is required for QApplication's argc/argv constructor
  char *defaultArgv[] = {defaultArgv0, nullptr};  // NOSONAR(cpp:S5421,cpp:S5954): This is required for QApplication's argc/argv constructor

  constexpr char k_tray_min_width_property[] = "tray_min_width";
  constexpr char k_tray_show_connected_property[] = "tray_show_connected";

  /**
   * @brief Trailing padding that reserves space for the native submenu indicator.
   *
   * @return Figure spaces wide enough to keep Qt's submenu arrow off the label text.
   */
  QString submenu_text_padding() {
    return QString(2, QChar(0x2007));
  }

  /**
   * @brief Apply the computed minimum width when a menu is shown.
   *
   * @param menu Menu about to be displayed.
   */
  void apply_menu_width_on_show(QMenu *menu) {
    if (!menu) {
      return;
    }

    const int min_width = menu->property(k_tray_min_width_property).toInt();
    if (min_width <= 0) {
      return;
    }

    const int target_width = std::max(min_width, menu->sizeHint().width());
    menu->setMinimumWidth(target_width);
  }

  /**
   * @brief Reserve horizontal space for submenu indicators and long labels.
   *
   * @param menu Menu to adjust.
   */
  void adjustMenuLayout(QMenu *menu) {
    if (!menu) {
      return;
    }

    const QFontMetrics fm {menu->fontMetrics()};
    const QStyle *style = menu->style();
    const int h_margin = style->pixelMetric(QStyle::PM_MenuHMargin, nullptr, menu) * 2;

    int min_width = 0;

    for (QAction *action : menu->actions()) {
      if (action->isSeparator()) {
        continue;
      }

      // Submenu labels already include trailing figure-space padding in createMenu().
      const int text_width = fm.boundingRect(action->text()).width();
      int item_width = text_width + h_margin;

      if (QMenu *child = action->menu()) {
        adjustMenuLayout(child);
      }

      min_width = std::max(min_width, item_width);
    }

    menu->setProperty(k_tray_min_width_property, min_width);
    menu->setMinimumWidth(min_width);

    if (!menu->property(k_tray_show_connected_property).toBool()) {
      menu->setProperty(k_tray_show_connected_property, true);

      QObject::connect(menu, &QMenu::aboutToShow, menu, [menu]() {
        apply_menu_width_on_show(menu);
      });
    }
  }
}  // namespace

QtTrayMenu::QtTrayMenu(QObject *parent, const bool debug):
    QtTrayMenu(-1, nullptr, parent, debug) {
    };

QtTrayMenu::QtTrayMenu(int argc, char **argv, QObject *parent, const bool debug):
    QObject(parent) {
  if (QApplication::instance()) {
    app = dynamic_cast<QApplication *>(QApplication::instance());
    if (!app) {
      qDebug() << "QCoreApplication is not a QApplication, please contact support.";
    }
  } else {
    // Note: The following is ugly but QApplication requires an argv containing the application name.
    // We might not have access to the real argc/argv here due to being called/pulled as a dependency.
    if (argc < 0 && argv == nullptr) {
      app = new QApplication(defaultArgc, defaultArgv);  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
    } else {
      app = new QApplication(argc, argv);  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
    }
  }
  if (debug) {
    app->installEventFilter(this);
  }
}

QtTrayMenu::~QtTrayMenu() {
  // Cleanup app only if it was created within this class
  if (app && app != QApplication::instance()) {
    // Quit QApplication
    QApplication::quit();
    // Delete app and clear references
    delete app;  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
    app = nullptr;  // Set to nullptr after deletion
  }
}

int QtTrayMenu::init(struct tray *tray, const bool notification) {
  if (trayIcon) {
    // Running tray is initialized again. Fail with error.
    return -1;
  }
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    // Qt does not support system tray. Fail with error.
    return -1;
  }

  this->trayStruct = tray;
  this->running = true;

  if (QApplication::applicationName().isEmpty() || QApplication::applicationName() == "TrayMenuApp") {
    QApplication::setApplicationName(tray->tooltip);
  }

  // Create tray icon
  trayIcon = new QSystemTrayIcon(lookupIcon(tray->icon), this);
  trayIcon->setToolTip(QString::fromUtf8(tray->tooltip));

  connect(trayIcon, &QSystemTrayIcon::activated, this, &QtTrayMenu::onTrayActivated);
  connect(trayIcon, &QSystemTrayIcon::messageClicked, this, &QtTrayMenu::onMessageClicked);
  connect(this, &QtTrayMenu::update, this, &QtTrayMenu::onUpdate);
  connect(this, &QtTrayMenu::exit, this, &QtTrayMenu::onExitRequested);
  connect(this, &QtTrayMenu::showMenu, this, &QtTrayMenu::onShowMenu);

  updateMenu(tray->menu);

  trayIcon->setContextMenu(trayTopMenu);
  trayIcon->show();

  if (notification) {
    createNotification();
  }

  return 0;
}

void QtTrayMenu::onUpdate(struct tray *tray, const bool notify) {
  if (!trayIcon) {
    return;
  }
  this->trayStruct = tray;
  if (const auto newIcon = QIcon(trayStruct->icon); !newIcon.isNull()) {
    trayIcon->setIcon(newIcon);
  }
  trayIcon->setToolTip(QString::fromUtf8(trayStruct->tooltip));

  updateMenu(trayStruct->menu);
  if (notify) {
    createNotification();
  }
}

int QtTrayMenu::loop(int blocking) {
  if (!running) {
    return -1;
  }
  if (!app || QApplication::closingDown()) {
    qDebug() << "Application is not in a valid state or is closing down.";
    return -1;
  }
  if (blocking) {
    blockingEventLoop = true;
    QApplication::exec();
    return -1;
  } else {
    blockingEventLoop = false;
    QApplication::processEvents();
    return 0;
  }
}

void QtTrayMenu::onExitRequested() {
  // Mark as no longer running
  running = false;
  // Remove tray menu references
  if (trayTopMenu) {
    trayTopMenu->hide();
    if (trayIcon) {
      trayIcon->setContextMenu(nullptr);
    }
    delete trayTopMenu;  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
    trayTopMenu = nullptr;  // Set to nullptr after deletion
  }
  // Remove tray icon references;
  if (trayIcon) {
    trayIcon->hide();
    delete trayIcon;  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
    trayIcon = nullptr;  // Set to nullptr after deletion
  }
  // Unset tray structure
  trayStruct = nullptr;

  // If we run in a blocking event loop break said loop by quitting the QApplication
  if (blockingEventLoop) {
    QApplication::quit();
  }
}

void QtTrayMenu::updateMenu(struct tray_menu *items) {
  // Create and setup new tray menu instance
  const auto newTrayTopMenu = new QMenu();  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
  trayIcon->setContextMenu(newTrayTopMenu);
  // Fill new tray menu instance
  createMenu(items, newTrayTopMenu);
  adjustMenuLayout(newTrayTopMenu);
  // Clear old, unused trayTopMenu instance
  if (trayTopMenu != nullptr) {
    trayTopMenu->clear();  // Remove all actions
    delete trayTopMenu;  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
  }
  // Store reference for cleanup
  trayTopMenu = newTrayTopMenu;
}

void QtTrayMenu::createMenu(struct tray_menu *items, QMenu *menu) {
  while (items && items->text) {
    if (strcmp(items->text, "-") == 0) {
      menu->addSeparator();
    } else {
      if (items->submenu) {
        const auto sub_menu = menu->addMenu(QString::fromUtf8(items->text) + submenu_text_padding());
        createMenu(items->submenu, sub_menu);
      } else {
        auto *action = new QAction(QString::fromUtf8(items->text), menu);  // NOSONAR(cpp:S5025) - Qt has its own integrated memory management
        action->setDisabled(items->disabled == 1);
        action->setCheckable(items->checkbox == 1);
        action->setChecked(items->checked == 1);
        action->setProperty("tray_menu_item", QVariant::fromValue((void *) items));
        connect(action, &QAction::triggered, this, &QtTrayMenu::onMenuItemTriggered);
        menu->addAction(action);
      }
    }
    items++;
  }
}

void QtTrayMenu::createNotification() {
  if (trayStruct && trayStruct->notification_title && trayStruct->notification_text) {
    const auto title = QString::fromUtf8(trayStruct->notification_title);
    const auto text = QString::fromUtf8(trayStruct->notification_text);
    if (trayStruct->notification_icon) {
      showMessage(title, text, trayStruct->notification_icon, trayStruct->notification_cb);
    } else {
      showMessage(title, text, trayStruct->notification_cb);
    }
  }
}

QIcon QtTrayMenu::lookupIcon(QString icon) const {
  // Find icon for tray
  if (std::filesystem::exists(icon.toStdString())) {
    if (auto result = QIcon(icon); !result.isNull()) {
      return result;
    }
  }
  if (auto result = QIcon::fromTheme(icon); !result.isNull()) {
    return result;
  }
  return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
}

bool QtTrayMenu::eventFilter(QObject *watched, QEvent *event) {
  qDebug() << "Event Type:" << event->type();
  return QObject::eventFilter(watched, event);
}

void QtTrayMenu::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
  if (reason != QSystemTrayIcon::Trigger) {
    return;
  }
  if (trayStruct && trayStruct->cb) {
    trayStruct->cb(trayStruct);
  } else {
    showMenu();
  }
}

void QtTrayMenu::onMenuItemTriggered() {
  auto *action = qobject_cast<QAction *>(sender());
  struct tray_menu *menuItem = getTrayMenuItem(action);

  if (menuItem && menuItem->cb) {
    menuItem->cb(menuItem);
  }
}

struct tray_menu *QtTrayMenu::getTrayMenuItem(QAction *action) {  // NOSONAR(cpp:S995) - Use as defined in function interface
  return static_cast<struct tray_menu *>(action->property("tray_menu_item").value<void *>());
}

void QtTrayMenu::onMessageClicked() const {
  if (notificationCallback != nullptr) {
    notificationCallback();
  }
}

void QtTrayMenu::configureAppMetadata(const QString &appName, const QString &appDisplayName, const QString &desktopName) const {
  const QString effective_name = !appName.isEmpty() ? appName : QStringLiteral("tray");
  if (QApplication::applicationName().isEmpty()) {
    QApplication::setApplicationName(effective_name);
  }

  if (QApplication::applicationDisplayName().isEmpty()) {
    if (!appDisplayName.isEmpty()) {
      QApplication::setApplicationDisplayName(appDisplayName);
    } else {
      const QString display_name =
        (trayStruct && trayStruct->tooltip) ? QString::fromUtf8(trayStruct->tooltip) : effective_name;
      QApplication::setApplicationDisplayName(display_name);
    }
  }

  if (!QApplication::desktopFileName().isEmpty()) {
    return;
  }

  if (!desktopName.isEmpty()) {
    QApplication::setDesktopFileName(desktopName);
    return;
  }

  QString desktop_name = QApplication::applicationName();
  if (!desktop_name.endsWith(QStringLiteral(".desktop"))) {
    desktop_name += QStringLiteral(".desktop");
  }
  QApplication::setDesktopFileName(desktop_name);
}

void QtTrayMenu::onShowMenu() const {
  if (!trayIcon) {
    return;
  }
  if (QMenu *menu = trayIcon->contextMenu(); menu != nullptr) {
    // QTBUG-139921: popup() fails on Linux/Wayland with Qt 6.9+ without a transient parent.
    menu->exec(QCursor::pos());
  }
}

bool QtTrayMenu::supportsMessages() {
  return QSystemTrayIcon::supportsMessages();
}

void QtTrayMenu::showMessage(const QString &title, const QString &msg, std::function<void()> callback, const QSystemTrayIcon::MessageIcon icon, const int msecs) {
  if (!trayIcon) {
    return;
  }
  if (QSystemTrayIcon::supportsMessages()) {
    notificationCallback = std::move(callback);
    emit trayIcon->showMessage(title, msg, icon, msecs);
  }
}

void QtTrayMenu::showMessage(const QString &title, const QString &msg, const QString &iconPath, std::function<void()> callback, const int msecs) {
  if (!trayIcon) {
    return;
  }
  if (QSystemTrayIcon::supportsMessages()) {
    notificationCallback = std::move(callback);
    emit trayIcon->showMessage(title, msg, lookupIcon(iconPath), msecs);
  }
}

void QtTrayMenu::clickMenuItem(int index) const {
  if (!trayIcon) {
    return;
  }
  const QMenu *menu = trayIcon->contextMenu();
  if (!menu) {
    return;
  }
  const QList<QAction *> actions = menu->actions();
  if (index < 0 || index >= actions.size()) {
    return;
  }
  QAction *action = actions.at(index);
  if (!action || action->isSeparator() || action->menu() != nullptr || !action->isEnabled()) {
    return;
  }
  emit action->trigger();
}

void QtTrayMenu::clickMessage() const {
  if (!trayIcon) {
    return;
  }
  emit trayIcon->messageClicked();
}
