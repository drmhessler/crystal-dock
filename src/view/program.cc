/*
 * This file is part of Crystal Dock.
 * Copyright (C) 2022 Viet Dang (dangvd@gmail.com)
 *
 * Crystal Dock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Crystal Dock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Crystal Dock.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "program.h"

#include <iostream>

#include <QDir>
#include <QGuiApplication>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTimer>

#include "display/window_system.h"

#include "dock_panel.h"
#include <utils/draw_utils.h>

namespace crystaldock {

Program::Program(DockPanel* parent, MultiDockModel* model, const QString& appId,
                 const QString& label, Qt::Orientation orientation, const QPixmap& icon,
                 int minSize, int maxSize, const QString& command, bool isAppMenuEntry,
                 bool pinned)
    : IconBasedDockItem(parent, model, label, orientation, icon, minSize, maxSize),
      appId_(appId),
      command_(command),
      isAppMenuEntry_(isAppMenuEntry),
      pinned_(pinned),
      demandsAttention_(false),
      attentionStrong_(false),
      launching_(false) {
  init();
}

Program::Program(DockPanel* parent, MultiDockModel* model, const QString& appId,
                 const QString& label, Qt::Orientation orientation, const QPixmap& icon,
                 int minSize, int maxSize)
    : IconBasedDockItem(parent, model, label, orientation, icon, minSize, maxSize),
      appId_(appId),
      command_(""),
      isAppMenuEntry_(false),
      pinned_(false),
      demandsAttention_(false),
      attentionStrong_(false),
      launching_(false) {
  init();
}

void Program::init() {
  createMenu();

  animationTimer_.setInterval(500);
  connect(&animationTimer_, &QTimer::timeout, this, [this]() {
    attentionStrong_ = !attentionStrong_;
    parent_->update();
  });

  bounceTimer_.setInterval(kBounceIntervalMs);
  connect(&bounceTimer_, &QTimer::timeout, this, &Program::updateBounceAnimation);
}

void Program::draw(QPainter *painter) const {
  painter->setRenderHint(QPainter::Antialiasing);
  auto taskCount = static_cast<int>(tasks_.size());
  if (taskCount == 0 && launching_) { taskCount = 1; }
  if (parent_->showTaskManager() && taskCount > 0) {  // Show task count indicator.
    static constexpr int kMaxVisibleTaskCount = 4;
    if (taskCount > kMaxVisibleTaskCount) { taskCount = kMaxVisibleTaskCount; }
    auto activeTask = getActiveTask();
    if (activeTask > kMaxVisibleTaskCount - 1) { activeTask = kMaxVisibleTaskCount - 1; }

    // Size (width if horizontal, or height if vertical) of the indicator.
    const int size = parent_->is3D() ? DockPanel::kIndicatorSize3D
                                     : parent_->isFlat2D() ? DockPanel::kIndicatorSizeFlat2D
                                                           : DockPanel::kIndicatorSizeMetal2D;
    const auto spacing = DockPanel::kIndicatorSpacing;
    const auto totalSize = taskCount * size + (taskCount - 1) * spacing;
    auto x = left_ + (getWidth() - totalSize) / 2 + size / 2;
    auto y = top_ + (getHeight() - totalSize) / 2 + size / 2;
    for (int i = 0; i < taskCount; ++i) {
      // If bouncing launcher icon is not enabled, we use active color
      // to provide feedback.
      bool useActiveColor = (i == activeTask) || attentionStrong_
          || (launching_ && !model_->bouncingLauncherIcon());
      if (parent_->is3D()) {
        const auto baseColor = useActiveColor
            ? model_->activeIndicatorColor() : model_->inactiveIndicatorColor();
        drawIndicator(orientation_, x, parent_->taskIndicatorPos(),
                      parent_->taskIndicatorPos(), y,
                      size, DockPanel::k3DPanelThickness, baseColor, painter);
      } else if (parent_->isFlat2D()) {
        const auto baseColor = useActiveColor
            ? model_->activeIndicatorColor2D() : model_->inactiveIndicatorColor2D();
        drawIndicatorFlat2D(orientation_, x, parent_->taskIndicatorPos(),
                            parent_->taskIndicatorPos(), y,
                            size, baseColor, painter);
      } else {  // Metal 2D.
          const auto baseColor = useActiveColor
              ? model_->activeIndicatorColorMetal2D() : model_->inactiveIndicatorColorMetal2D();
          drawIndicatorMetal2D(parent_->position(), x, parent_->taskIndicatorPos(),
                               parent_->taskIndicatorPos(), y,
                               size, baseColor, painter);
      }
      x += (size + spacing);
      y += (size + spacing);
    }
  }
  painter->setRenderHint(QPainter::Antialiasing, false);

  IconBasedDockItem::draw(painter);
}

void Program::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) { // Run the application.
    if (appId_ == kLockScreenId) {
      parent_->leaveEvent(nullptr);
      QTimer::singleShot(DockPanel::kMenuPopupDelayMs, [this]() {
        launch();
      });
    } else {
      if (tasks_.empty()) {
        launch();
        startBounceAnimation();
      } else {
        const auto mod = QGuiApplication::keyboardModifiers();
        if (mod & Qt::ShiftModifier) {
          launch();
          startBounceAnimation();
        } else if (tasks_.size() == 1) {
          WindowSystem::activateOrMinimizeWindow(tasks_[0].uuid);
        } else {
          const auto activeTask = getActiveTask();
          if (activeTask >= 0) {
            // Cycles through tasks.
            auto lastTask = static_cast<int>(tasks_.size() - 1);
            if (mod & Qt::ControlModifier) {
              // Cycles backwards with CTRL.
              auto nextTask = activeTask > 0 ? (activeTask - 1) : lastTask;
              WindowSystem::activateWindow(tasks_[nextTask].uuid);
            } else {
              auto nextTask = (activeTask < lastTask) ? (activeTask + 1) : 0;
              WindowSystem::activateWindow(tasks_[nextTask].uuid);
            }
          } else {
            for (unsigned i = 0; i < tasks_.size(); ++i) {
              WindowSystem::activateWindow(tasks_[i].uuid);
            }
          }
        }
      }
    }
  } else if (e->button() == Qt::RightButton) {
      parent_->minimize();
      QTimer::singleShot(DockPanel::kMenuPopupDelayMs, [this]{ menu_.exec(parent_->mapToGlobal(QPoint(left_, top_))); });
  }
}

QString Program::getLabel() const {
  const unsigned taskCount = tasks_.size();
  return (taskCount > 1) ?
      label_ + " (" + QString::number(tasks_.size()) + " windows)" :
      label_;
}

bool Program::addTask(const WindowInfo* task) {
  auto* app = model_->findApplication(task->appId);
  if ((app && app->appId == appId_) || task->appId == appId_.toStdString()) {
    tasks_.push_back(ProgramTask(task->uuid, QString::fromStdString(task->title),
                                 task->demandsAttention));
    if (task->demandsAttention) {
      setDemandsAttention(true);
    }
    updateMenu();
    return true;
  }
  return false;
}

bool Program::updateTask(const WindowInfo* task) {
  if (task->appId != appId_.toStdString()) {
    return false;
  }

  for (auto& existingTask : tasks_) {
    if (existingTask.uuid == task->uuid) {
      existingTask.demandsAttention = task->demandsAttention;
      updateDemandsAttention();
      return true;
    }
  }

  return false;
}

bool Program::removeTask(std::string_view uuid) {
  for (int i = 0; i < static_cast<int>(tasks_.size()); ++i) {
    if (tasks_[i].uuid == uuid) {
      tasks_.erase(tasks_.begin() + i);
      updateMenu();
      return true;
    }
  }
  return false;
}

bool Program::hasTask(std::string_view uuid) {
  for (const auto& task : tasks_) {
    if (task.uuid == uuid) {
      return true;
    }
  }
  return false;
}

bool Program::beforeTask(const QString& program) {
  return pinned_ || label_ < program;
}

void Program::launch() {
  launching_ = true;
  parent_->update();
  launch(command_);
  QTimer::singleShot(kLaunchingAcknowledgementDurationMs,
                     [this] {
                       launching_ = false; parent_->update();
                     });
}

void Program::pinUnpin() {
  pinned_ = !pinned_;
  if (pinned_) {
    model_->addLauncher(parent_->dockId(), LauncherConfig(appId_, label_, iconName_, command_));
  } else {  // !pinned
    model_->removeLauncher(parent_->dockId(), appId_);
    if (shouldBeRemoved()) {
      parent_->delayedRefresh();
    }
  }
}

void Program::launch(const QString& command) {
  QStringList list = QProcess::splitCommand(command);
  QProcess process;
  process.setProgram(list.at(0));
  process.setArguments(list.mid(1));
  auto env = QProcessEnvironment::systemEnvironment();
  // Unset XDG_ACTIVATION_TOKEN.
  env.insert("XDG_ACTIVATION_TOKEN", "");
  // Unset layer-shell env.
  env.insert("QT_WAYLAND_SHELL_INTEGRATION", "");
  process.setProcessEnvironment(env);
  process.setWorkingDirectory(QDir::homePath());
  if (!process.startDetached()) {
    QMessageBox warning(QMessageBox::Warning, "Error",
                        QString("Could not run command: ") + command,
                        QMessageBox::Ok, nullptr, Qt::Tool);
    warning.exec();
  }
}

void Program::closeAllWindows() {
  for (const auto& task : tasks_) {
    WindowSystem::closeWindow(task.uuid);
  }
}

void Program::createMenu() {
  menu_.addSection(QIcon::fromTheme(iconName_), label_);

  if (isAppMenuEntry_ || pinned_) {
    pinAction_ = menu_.addAction(
        QString("Pinned"), this,
        [this] {
          pinUnpin();
        });
    pinAction_->setCheckable(true);
    pinAction_->setChecked(pinned_);
  }

  if (isAppMenuEntry_) {
    menu_.addAction(QIcon::fromTheme("list-add"), QString("&New Window"), this,
                    [this] { launch(); });
  }

  closeAction_ = menu_.addAction(QIcon::fromTheme("window-close"), QString("&Close Window"), this,
                                 [this] { closeAllWindows(); });

  menu_.addSeparator();
  menu_.addAction(QIcon::fromTheme("configure"), QString("Edit &Launchers"), parent_,
                  [this] { parent_->showEditLaunchersDialog(); });

  if (model_->showTaskManager(parent_->dockId())) {
    menu_.addAction(QIcon::fromTheme("configure"),
                    QString("Task Manager &Settings"),
                    parent_,
                    [this] { parent_->showTaskManagerSettingsDialog(); });
  }

  menu_.addSeparator();
  parent_->addPanelSettings(&menu_);
  updateMenu();
}

void Program::setDemandsAttention(bool value) {
  if (demandsAttention_ == value) {
    return;
  }

  demandsAttention_ = value;
  if (demandsAttention_) {
    animationTimer_.start();
  } else if (animationTimer_.isActive()) {
    animationTimer_.stop();
    attentionStrong_ = false;
  }
  parent_->update();
}

void Program::updateDemandsAttention() {
  for (const auto& task : tasks_) {
    if (task.demandsAttention) {
      setDemandsAttention(true);
      return;
    }
  }
  setDemandsAttention(false);
}

void Program::updateMenu() {
  closeAction_->setVisible(!tasks_.empty());
  closeAction_->setText(tasks_.size() > 1 ? "&Close All Windows" : "&Close Window");
}

void Program::startBounceAnimation() {
  if (!model_->bouncingLauncherIcon()) {
    return;
  }

  if (!bouncing_) {
    bouncing_ = true;
    bouncingUp_ = true;
    bounceProgress_ = 0.0f;
    setAnimationStartAsCurrent();
    bounceTimer_.start();
  }
}

void Program::updateBounceAnimation() {
  if (!bouncing_) {
    return;
  }

  float bounceStep = 1.0f / kBounceSteps;
  float nextBounceRatio = bounceProgress_ + bounceStep;
  if (nextBounceRatio < 1.0f) {
    bounceProgress_ = nextBounceRatio;
  } else {
    if (!bouncingUp_) {
      // Done and done
      bounceProgress_ = 1.0f;
      bouncing_ = false;
      bounceTimer_.stop();

      return;
    }

    // It was bouncing up
    bounceProgress_ = 0.0f;
    bouncingUp_ = false;
  }

  float bounceOffset = getBounceOffset();
  endTop_ = startTop_ + bounceOffset;
  endLeft_ = startLeft_;
  endSize_ = startSize_;
  startAnimation(1);
  nextAnimationStep();
  parent_->update();
}

float Program::getBounceOffset() const {
  float bounceOffset;
  if (bouncingUp_) {
    float ratio = 1.0f - std::pow(1.0f - bounceProgress_, kBounceEaseOut);
    bounceOffset = -kBounceHeight * ratio;
  } else {
    float ratio = std::pow(bounceProgress_, kBounceEaseIn);
    bounceOffset = -kBounceHeight * (1.0f - ratio);
  }

  return bounceOffset;
}

}  // namespace crystaldock
