/* This file is part of the KDE project
 *
 *  Copyright 2019 Dominik Haumann <dhaumann@kde.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */
#include "externaltoolsplugin.h"

#include "kateexternaltool.h"
#include "kateexternaltoolscommand.h"
#include "katemacroexpander.h"
#include "katetoolrunner.h"
#include "kateexternaltoolsconfigwidget.h"
#include "externaltools.h"

#include <KLocalizedString>
#include <KTextEditor/Editor>
#include <KTextEditor/View>

#include <KActionCollection>
#include <QAction>
#include <kparts/part.h>

#include <KMessageBox>
#include <KAboutData>
#include <KAuthorized>
#include <KConfig>
#include <KConfigGroup>
#include <KPluginFactory>
#include <KXMLGUIFactory>

K_PLUGIN_FACTORY_WITH_JSON(KateExternalToolsFactory, "externaltoolsplugin.json",
                           registerPlugin<KateExternalToolsPlugin>();)

KateExternalToolsPlugin::KateExternalToolsPlugin(QObject* parent, const QList<QVariant>&)
    : KTextEditor::Plugin(parent)
{
    reload();
}

KateExternalToolsPlugin::~KateExternalToolsPlugin()
{
    delete m_command;
    m_command = nullptr;
}

QObject* KateExternalToolsPlugin::createView(KTextEditor::MainWindow* mainWindow)
{
    KateExternalToolsPluginView* view = new KateExternalToolsPluginView(mainWindow, this);
    connect(this, &KateExternalToolsPlugin::externalToolsChanged, view, &KateExternalToolsPluginView::rebuildMenu);
    return view;
}

void KateExternalToolsPlugin::reload()
{
    m_commands.clear();

    KConfig _config(QStringLiteral("externaltools"), KConfig::NoGlobals, QStandardPaths::ApplicationsLocation);
    KConfigGroup config(&_config, "Global");
    const QStringList tools = config.readEntry("tools", QStringList());

    for (QStringList::const_iterator it = tools.begin(); it != tools.end(); ++it) {
        if (*it == QStringLiteral("---"))
            continue;

        config = KConfigGroup(&_config, *it);

        auto t = new KateExternalTool();
        t->load(config);
        m_tools.push_back(t);

        // FIXME test for a command name first!
        if (t->hasexec && (!t->cmdname.isEmpty())) {
            m_commands.push_back(t->cmdname);
        }
    }

    if (KAuthorized::authorizeAction(QStringLiteral("shell_access"))) {
        delete m_command;
        m_command = new KateExternalToolsCommand(this);
    }

    Q_EMIT externalToolsChanged();
}

QStringList KateExternalToolsPlugin::commands() const
{
    return m_commands;
}

const KateExternalTool* KateExternalToolsPlugin::toolForCommand(const QString& cmd) const
{
    for (auto tool : m_tools) {
        if (tool->cmdname == cmd) {
            return tool;
        }
    }
    return nullptr;
}

const QVector<KateExternalTool*> KateExternalToolsPlugin::tools() const
{
    return m_tools;
}

void KateExternalToolsPlugin::runTool(const KateExternalTool& tool, KTextEditor::View* view)
{
    // expand the macros in command if any,
    // and construct a command with an absolute path
    auto mw = view->mainWindow();

    // save documents if requested
    if (tool.saveMode == KateExternalTool::SaveMode::CurrentDocument) {
        view->document()->save();
    } else if (tool.saveMode == KateExternalTool::SaveMode::AllDocuments) {
        foreach (KXMLGUIClient* client, mw->guiFactory()->clients()) {
            if (QAction* a = client->actionCollection()->action(QStringLiteral("file_save_all"))) {
                a->trigger();
                break;
            }
        }
    }

    // copy tool
    auto copy = new KateExternalTool(tool);

    MacroExpander macroExpander(view);
    if (!macroExpander.expandMacrosShellQuote(copy->arguments)) {
        KMessageBox::sorry(view, i18n("Failed to expand the arguments '%1'.", copy->arguments),
                           i18n("Kate External Tools"));
        return;
    }

    if (!macroExpander.expandMacrosShellQuote(copy->workingDir)) {
        KMessageBox::sorry(view, i18n("Failed to expand the working directory '%1'.", copy->workingDir),
                           i18n("Kate External Tools"));
        return;
    }

    // Allocate runner on heap such that it lives as long as the child
    // process is running and does not block the main thread.
    auto runner = new KateToolRunner(copy, this);
    connect(runner, &KateToolRunner::toolFinished, this, &KateExternalToolsPlugin::handleToolFinished);
    runner->run();
}

void KateExternalToolsPlugin::handleToolFinished(KateToolRunner* runner)
{
    runner->deleteLater();
}

int KateExternalToolsPlugin::configPages() const
{
    return 1;
}

KTextEditor::ConfigPage* KateExternalToolsPlugin::configPage(int number, QWidget* parent)
{
    if (number == 0) {
        return new KateExternalToolsConfigWidget(parent, this);
    }
    return nullptr;
}

KateExternalToolsPluginView::KateExternalToolsPluginView(KTextEditor::MainWindow* mainWindow,
                                                         KateExternalToolsPlugin* plugin)
    : QObject(mainWindow)
    , m_plugin(plugin)
    , m_mainWindow(mainWindow)
{
    KXMLGUIClient::setComponentName(QLatin1String("externaltools"), i18n("External Tools"));
    setXMLFile(QLatin1String("ui.rc"));

    if (KAuthorized::authorizeAction(QStringLiteral("shell_access"))) {
        m_externalToolsMenu = new KateExternalToolsMenuAction(i18n("External Tools"), actionCollection(), plugin, mainWindow);
        actionCollection()->addAction(QStringLiteral("tools_external"), m_externalToolsMenu);
        m_externalToolsMenu->setWhatsThis(i18n("Launch external helper applications"));
    }

    mainWindow->guiFactory()->addClient(this);
}

void KateExternalToolsPluginView::rebuildMenu()
{
    if (m_externalToolsMenu) {
        KXMLGUIFactory* f = factory();
        f->removeClient(this);
        reloadXML();
        m_externalToolsMenu->reload();
        f->addClient(this);
    }
}

KateExternalToolsPluginView::~KateExternalToolsPluginView()
{
    m_mainWindow->guiFactory()->removeClient(this);

    delete m_externalToolsMenu;
    m_externalToolsMenu = nullptr;
}

KTextEditor::MainWindow* KateExternalToolsPluginView::mainWindow() const
{
    return m_mainWindow;
}

#include "externaltoolsplugin.moc"

// kate: space-indent on; indent-width 4; replace-tabs on;
