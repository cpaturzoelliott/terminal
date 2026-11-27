// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "CommandHistoryControl.h"

#include "CommandHistoryControl.g.cpp"

#include "TerminalPage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace winrt::Windows::UI::Xaml;

namespace
{
    std::wstring _getEnvironmentVariable(const wchar_t* name)
    {
        const auto required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0)
        {
            return {};
        }

        std::wstring value(required, L'\0');
        const auto written = GetEnvironmentVariableW(name, value.data(), required);
        value.resize(written);
        return value;
    }

    // Method Description:
    // - Finds the history file that the shell of this profile writes to, if we
    //   know about that shell. PowerShell (5.1 and 7), bash and zsh are
    //   supported. Others (cmd, WSL shells, ...) have no file we can read.
    std::filesystem::path _getShellHistoryPath(const std::wstring_view commandline)
    {
        std::wstring lowered{ commandline };
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        const auto appData = _getEnvironmentVariable(L"APPDATA");
        const auto userProfile = _getEnvironmentVariable(L"USERPROFILE");

        if (lowered.find(L"pwsh") != std::wstring::npos)
        {
            return std::filesystem::path{ appData } / L"Microsoft" / L"PowerShell" / L"PSReadLine" / L"ConsoleHost_history.txt";
        }
        if (lowered.find(L"powershell") != std::wstring::npos)
        {
            return std::filesystem::path{ appData } / L"Microsoft" / L"Windows" / L"PowerShell" / L"PSReadLine" / L"ConsoleHost_history.txt";
        }
        if (lowered.find(L"bash") != std::wstring::npos)
        {
            return std::filesystem::path{ userProfile } / L".bash_history";
        }
        if (lowered.find(L"zsh") != std::wstring::npos)
        {
            return std::filesystem::path{ userProfile } / L".zsh_history";
        }

        return {};
    }

    std::wstring _toLower(const std::wstring_view text)
    {
        std::wstring lowered{ text };
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return lowered;
    }

    std::wstring _dedupeKey(const std::wstring_view text)
    {
        // Ignore case and surrounding whitespace, so that the same command
        // typed with (for example) a trailing space still counts as a duplicate.
        const auto first = text.find_first_not_of(L" \t");
        if (first == std::wstring_view::npos)
        {
            return _toLower(text);
        }

        const auto last = text.find_last_not_of(L" \t");
        return _toLower(text.substr(first, last - first + 1));
    }

    bool _containsIgnoreCase(const std::wstring_view text, const std::wstring_view needle)
    {
        if (needle.empty())
        {
            return true;
        }

        return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](wchar_t a, wchar_t b) {
                   return std::towlower(a) == std::towlower(b);
               }) != text.end();
    }

    // Method Description:
    // - The word that a command is grouped by: everything before the first
    //   space, lower cased so that "Git" and "git" group together.
    std::wstring _groupKey(const std::wstring_view command)
    {
        const auto word = command.substr(0, command.find_first_of(L" \t"));
        return word.empty() ? std::wstring{} : _toLower(word);
    }

    // Method Description:
    // - The first word of a command, as it was written. This titles a group.
    std::wstring _groupTitle(const std::wstring_view command)
    {
        return std::wstring{ command.substr(0, command.find_first_of(L" \t")) };
    }

    // Method Description:
    // - Escapes a command so that it fits on one line of the state file.
    std::string _escapeStateValue(const std::wstring_view text)
    {
        std::string escaped;
        for (const auto ch : til::u16u8(text))
        {
            switch (ch)
            {
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            default:
                escaped.push_back(ch);
                break;
            }
        }
        return escaped;
    }

    // Method Description:
    // - Reverses _escapeStateValue.
    std::wstring _unescapeStateValue(const std::string_view text)
    {
        std::string unescaped;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\\' && i + 1 < text.size())
            {
                switch (text[++i])
                {
                case 'n':
                    unescaped.push_back('\n');
                    break;
                case 'r':
                    unescaped.push_back('\r');
                    break;
                default:
                    unescaped.push_back(text[i]);
                    break;
                }
            }
            else
            {
                unescaped.push_back(text[i]);
            }
        }
        return til::u8u16(unescaped);
    }

    // Method Description:
    // - Finds the first descendant of the requested type, for the rows that need
    //   to look different from the template's default.
    template<typename T>
    T _findDescendant(const winrt::Windows::UI::Xaml::DependencyObject& root)
    {
        const auto count = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
        for (auto i = 0; i < count; ++i)
        {
            const auto child = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChild(root, i);
            if (const auto match = child.try_as<T>())
            {
                return match;
            }
            if (const auto nested = _findDescendant<T>(child))
            {
                return nested;
            }
        }
        return nullptr;
    }

    // The widget remembers its width and whether it was open between launches.
    constexpr double DefaultPanelWidth{ 340.0 };
    constexpr double MinPanelWidth{ 200.0 };
    constexpr double MaxPanelWidth{ 800.0 };

    // How long a second click on the same entry still counts as a double click.
    // This is generous on purpose: a slightly slow double click shouldn't be read
    // as two separate clicks that put the command on the command line twice.
    constexpr uint64_t DoubleClickWindow{ 1500 };

    // Method Description:
    // - Path of the file the widget stores its state in. It sits next to the
    //   user's settings.
    // Return Value:
    // - The path, or an empty path if this app has no local folder.
    std::filesystem::path _getStatePath()
    {
        try
        {
            const auto folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
            return std::filesystem::path{ folder.Path().c_str() } / L"commandhistory-state.txt";
        }
        catch (...)
        {
            return {};
        }
    }

    // Method Description:
    // - Reads a checkbox state. An unset state is not checked.
    bool _checked(const winrt::Windows::Foundation::IReference<bool>& value)
    {
        return value && value.Value();
    }

    int _compareIgnoreCase(const std::wstring_view a, const std::wstring_view b)
    {
        const auto result = CompareStringOrdinal(a.data(),
                                                 static_cast<int>(a.size()),
                                                 b.data(),
                                                 static_cast<int>(b.size()),
                                                 TRUE);
        if (result == CSTR_LESS_THAN)
        {
            return -1;
        }
        return result == CSTR_GREATER_THAN ? 1 : 0;
    }
}

namespace winrt::TerminalApp::implementation
{
    CommandHistoryControl::CommandHistoryControl()
    {
        InitializeComponent();

        _items = winrt::single_threaded_observable_vector<winrt::hstring>();
        CommandHistoryListView().ItemsSource(_items);

        // The entries contain selectable text, which swallows the ListView's
        // ItemClick. Listen to the pointer events directly (including the ones
        // the text control marks as handled), so that a click still activates
        // an entry while a drag selects text instead.
        CommandHistoryListView().AddHandler(
            UIElement::PointerPressedEvent(),
            winrt::box_value(Windows::UI::Xaml::Input::PointerEventHandler{ get_weak(), &CommandHistoryControl::_PointerPressed }),
            true);
        CommandHistoryListView().AddHandler(
            UIElement::PointerReleasedEvent(),
            winrt::box_value(Windows::UI::Xaml::Input::PointerEventHandler{ get_weak(), &CommandHistoryControl::_PointerReleased }),
            true);

        _refreshTimer.Interval(std::chrono::milliseconds{ 500 });
        _refreshTimer.Tick({ get_weak(), &CommandHistoryControl::_RefreshTimerTick });

        CommandHistoryListView().ContainerContentChanging({ get_weak(), &CommandHistoryControl::_ContainerContentChanging });

        _LoadState();
    }

    // Method Description:
    // - Finds the terminal that this widget's page currently has focused.
    // Return Value:
    // - The active terminal, or nullptr if there isn't one (yet).
    winrt::Microsoft::Terminal::Control::TermControl CommandHistoryControl::_ActiveTerminal() const
    {
        // The widget lives in a TerminalPage, which knows which pane is active.
        auto element = Parent();
        while (element)
        {
            if (const auto page = element.try_as<winrt::TerminalApp::TerminalPage>())
            {
                return winrt::get_self<TerminalPage>(page)->_GetActiveControl();
            }

            element = Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(element);
        }

        return nullptr;
    }

    // Method Description:
    // - Reads the command history that the shell persists to disk, oldest
    //   first. The file is only re-read when it changed since the last time we
    //   looked at it.
    // Arguments:
    // - commandline: the command line of the profile, used to find the history
    //   file of the shell it starts.
    // Return Value:
    // - The shell's commands, oldest first. Empty if there's no file to read.
    std::vector<winrt::hstring> CommandHistoryControl::_ReadShellHistory(const winrt::hstring commandline)
    {
        const auto path = _getShellHistoryPath(commandline);

        std::error_code ec;
        const auto lastWrite = path.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
        if (path.empty() || ec)
        {
            _historyPath.clear();
            _historyWriteTime = 0;
            _historyCommands.clear();
            return {};
        }

        const auto pathString = path.native();
        const auto writeTime = static_cast<uint64_t>(lastWrite.time_since_epoch().count());
        if (pathString == _historyPath && writeTime == _historyWriteTime)
        {
            return _historyCommands;
        }

        _historyPath = pathString;
        _historyWriteTime = writeTime;
        _historyCommands.clear();

        std::ifstream file{ path, std::ios::binary };
        if (!file.is_open())
        {
            return _historyCommands;
        }

        const auto isZsh = path.filename() == L".zsh_history";

        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            if (_historyCommands.empty() && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
            {
                line.erase(0, 3);
            }

            if (line.empty())
            {
                continue;
            }

            // bash writes "#<timestamp>" lines when HISTTIMEFORMAT is set.
            if (line.size() > 1 && line[0] == '#' && std::isdigit(static_cast<unsigned char>(line[1])))
            {
                continue;
            }

            std::wstring command;
            try
            {
                command = til::u8u16(line);
            }
            catch (...)
            {
                continue;
            }

            if (isZsh)
            {
                // zsh writes ": 1234567890:0;command"
                const auto semicolon = command.find(L';');
                if (semicolon == std::wstring::npos)
                {
                    continue;
                }
                command = command.substr(semicolon + 1);
            }

            if (!command.empty())
            {
                _historyCommands.push_back(winrt::hstring{ command });
            }
        }

        if (_historyCommands.size() > 1000)
        {
            _historyCommands.erase(_historyCommands.begin(), _historyCommands.end() - 1000);
        }

        return _historyCommands;
    }

    void CommandHistoryControl::Toggle()
    {
        _SetOpen(!_open);
    }

    // Method Description:
    // - Shows or hides the widget, keeping it refreshed while it is open.
    void CommandHistoryControl::_SetOpen(const bool open)
    {
        _open = open;
        Visibility(open ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed);

        if (open)
        {
            _Refresh();
            _refreshTimer.Start();
        }
        else
        {
            _refreshTimer.Stop();
        }

        _SaveState();
    }

    // Method Description:
    // - Forgets the entry that the last click inserted, so that the next click
    //   on it inserts it again rather than running it.
    void CommandHistoryControl::_ForgetInserted()
    {
        _lastInsertedCommand.clear();
        _lastInsertedTime = 0;
    }

    // Method Description:
    // - Restores the width and the open state that the widget had last time.
    // Return Value:
    // - <none>
    void CommandHistoryControl::_LoadState()
    {
        const auto path = _getStatePath();
        if (path.empty())
        {
            return;
        }

        std::ifstream file{ path };
        if (!file.is_open())
        {
            return;
        }

        // Restoring the checkbox values below reports them as changed, which
        // would write the file back out while it is still being read.
        _loadingState = true;

        auto width = DefaultPanelWidth;
        auto open = false;
        auto hideDuplicates = false;
        auto group = false;

        std::string line;
        while (std::getline(file, line))
        {
            const auto equals = line.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }

            const auto key = line.substr(0, equals);
            const auto value = line.substr(equals + 1);

            if (key == "width")
            {
                try
                {
                    width = std::clamp(std::stod(value), MinPanelWidth, MaxPanelWidth);
                }
                catch (...)
                {
                }
            }
            else if (key == "open")
            {
                open = value == "1";
            }
            else if (key == "duplicates")
            {
                hideDuplicates = value == "1";
            }
            else if (key == "group")
            {
                group = value == "1";
            }
            else if (key == "hidden")
            {
                try
                {
                    _hiddenCommands.insert(_unescapeStateValue(value));
                }
                catch (...)
                {
                }
            }
        }

        Width(width);
        HideDuplicatesCheckBox().IsChecked(hideDuplicates);
        GroupCheckBox().IsChecked(group);

        _loadingState = false;
        _SetOpen(open);
    }

    // Method Description:
    // - Writes the width and the open state out, so that the next launch starts
    //   the way this one ended.
    // Return Value:
    // - <none>
    void CommandHistoryControl::_SaveState()
    {
        if (_loadingState)
        {
            return;
        }

        const auto path = _getStatePath();
        if (path.empty())
        {
            return;
        }

        std::ofstream file{ path, std::ios::trunc };
        if (!file.is_open())
        {
            return;
        }

        file << "width=" << static_cast<int>(std::lround(Width())) << '\n'
             << "open=" << (_open ? 1 : 0) << '\n'
             << "duplicates=" << (_checked(HideDuplicatesCheckBox().IsChecked()) ? 1 : 0) << '\n'
             << "group=" << (_checked(GroupCheckBox().IsChecked()) ? 1 : 0) << '\n';

        for (const auto& command : _hiddenCommands)
        {
            file << "hidden=" << _escapeStateValue(command) << '\n';
        }
    }

    void CommandHistoryControl::_RefreshTimerTick(const Windows::Foundation::IInspectable& /*sender*/, const Windows::Foundation::IInspectable& /*args*/)
    {
        _Refresh();
    }

    void CommandHistoryControl::_Refresh()
    {
        if (!_open || !_items)
        {
            return;
        }

        std::vector<winrt::hstring> commands;

        try
        {
            if (const auto control{ _ActiveTerminal() })
            {
                // The shell's own history file has the commands from previous
                // sessions as well, so start from that.
                const auto settings{ control.Settings() };
                commands = _ReadShellHistory(settings ? settings.Commandline() : winrt::hstring{});

                // Then add the commands this terminal has seen, but that the
                // shell hasn't written to its file (yet).
                if (const auto context{ control.CommandHistory() })
                {
                    const auto history{ context.History() };
                    for (uint32_t i = 0; i < history.Size(); ++i)
                    {
                        auto command{ history.GetAt(i) };
                        if (std::find(commands.begin(), commands.end(), command) == commands.end())
                        {
                            commands.push_back(std::move(command));
                        }
                    }
                }
            }
        }
        catch (...)
        {
            commands.clear();
        }

        // Newest command first.
        std::reverse(commands.begin(), commands.end());

        bool changed = commands.size() != _commands.size();
        if (!changed)
        {
            for (size_t i = 0; i < commands.size(); ++i)
            {
                if (commands[i] != _commands[i])
                {
                    changed = true;
                    break;
                }
            }
        }

        if (changed)
        {
            _commands = std::move(commands);
        }

        _ApplyFilter();
    }

    // Method Description:
    // - Rebuilds the list from _commands, applying the search text, the
    //   duplicate filter and the sort order.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void CommandHistoryControl::_ApplyFilter()
    {
        if (!_items)
        {
            return;
        }

        const auto search = SearchBox().Text();
        const auto hideDuplicates = _checked(HideDuplicatesCheckBox().IsChecked());
        const auto group = _checked(GroupCheckBox().IsChecked());
        const auto sortAlphabetically = SortComboBox().SelectedIndex() == 1;

        std::vector<winrt::hstring> commands;
        commands.reserve(_commands.size());

        std::unordered_set<std::wstring> seen;
        for (const auto& command : _commands) // newest first
        {
            if (!_containsIgnoreCase(command, search))
            {
                continue;
            }

            // Commands that were removed from the list stay removed.
            if (_hiddenCommands.contains(_dedupeKey(command)))
            {
                continue;
            }

            // _commands is newest first, so this keeps the most recent usage.
            if (hideDuplicates && !seen.emplace(_dedupeKey(command)).second)
            {
                continue;
            }

            commands.push_back(command);
        }

        if (sortAlphabetically)
        {
            // Stable, so that the newest of two identical commands stays first.
            std::stable_sort(commands.begin(), commands.end(), [](const auto& a, const auto& b) {
                return _compareIgnoreCase(a, b) < 0;
            });
        }

        _rows.clear();
        _visibleGroups.clear();

        if (group)
        {
            // Commands that start with the same word go on one row that opens and
            // closes, but only when there are at least two of them. Everything
            // else stays a plain row.
            struct Group
            {
                std::wstring Title;
                std::vector<winrt::hstring> Members;
            };

            std::vector<std::wstring> keys;
            keys.reserve(commands.size());

            std::unordered_map<std::wstring, Group> groups;
            for (const auto& command : commands)
            {
                const auto key = _groupKey(command);
                keys.push_back(key);

                if (!key.empty())
                {
                    auto& group = groups[key];
                    if (group.Title.empty())
                    {
                        group.Title = _groupTitle(command);
                    }
                    group.Members.push_back(command);
                }
            }

            std::unordered_set<std::wstring> opened;
            for (size_t i = 0; i < commands.size(); ++i)
            {
                const auto& command = commands[i];
                const auto& key = keys[i];

                const auto it = key.empty() ? groups.end() : groups.find(key);
                if (it == groups.end() || it->second.Members.size() < 2)
                {
                    _rows.push_back(HistoryRow{ false, command, command, {} });
                    continue;
                }

                // The group goes where its newest command would have been.
                if (!opened.insert(key).second)
                {
                    continue;
                }

                const auto& members = it->second.Members;
                const auto isCollapsed = _collapsedGroups.contains(key);
                _visibleGroups.insert(key);

                std::wstring text{ isCollapsed ? L"\u25B8  " : L"\u25BE  " };
                text.append(it->second.Title);
                text.append(L"  (");
                text.append(std::to_wstring(members.size()));
                text.push_back(L')');

                _rows.push_back(HistoryRow{ true, winrt::hstring{ text }, {}, key });

                if (!isCollapsed)
                {
                    for (const auto& member : members)
                    {
                        _rows.push_back(HistoryRow{ false, member, member, {} });
                    }
                }
            }
        }
        else
        {
            for (const auto& command : commands)
            {
                _rows.push_back(HistoryRow{ false, command, command, {} });
            }
        }

        bool changed = _rows.size() != _items.Size();
        if (!changed)
        {
            for (uint32_t i = 0; i < static_cast<uint32_t>(_rows.size()); ++i)
            {
                if (_rows[i].Text != _items.GetAt(i))
                {
                    changed = true;
                    break;
                }
            }
        }

        if (changed)
        {
            _items.Clear();
            for (const auto& row : _rows)
            {
                _items.Append(row.Text);
            }
        }

        const auto visible = Windows::UI::Xaml::Visibility::Visible;
        const auto collapsed = Windows::UI::Xaml::Visibility::Collapsed;
        CommandHistoryEmptyText().Visibility(_commands.empty() ? visible : collapsed);
        CommandHistoryNoMatchesText().Visibility(!_commands.empty() && _rows.empty() ? visible : collapsed);
    }

    void CommandHistoryControl::_SearchTextChanged(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::Controls::TextChangedEventArgs& /*eventArgs*/)
    {
        _ApplyFilter();
    }

    void CommandHistoryControl::_FilterChanged(const Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& /*eventArgs*/)
    {
        _ApplyFilter();
        _SaveState();
    }

    void CommandHistoryControl::_SortChanged(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& /*eventArgs*/)
    {
        _ApplyFilter();
    }

    void CommandHistoryControl::_GroupChanged(const Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& /*eventArgs*/)
    {
        _ApplyFilter();
        _SaveState();
    }

    void CommandHistoryControl::_ExpandAllClick(const Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& /*eventArgs*/)
    {
        _collapsedGroups.clear();
        _ApplyFilter();
    }

    void CommandHistoryControl::_CollapseAllClick(const Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& /*eventArgs*/)
    {
        _collapsedGroups = _visibleGroups;
        _ApplyFilter();
    }

    void CommandHistoryControl::_RemoveClick(const Windows::Foundation::IInspectable& sender, const RoutedEventArgs& /*eventArgs*/)
    {
        const auto element = sender.try_as<FrameworkElement>();
        if (!element)
        {
            return;
        }

        const auto text = element.DataContext().try_as<winrt::Windows::Foundation::IReference<winrt::hstring>>();
        if (!text)
        {
            return;
        }

        // Removing only takes the command out of this list. The shell's history
        // file is left alone.
        _hiddenCommands.insert(_dedupeKey(text.Value()));
        _ForgetInserted();
        _ApplyFilter();
        _SaveState();
    }

    // Method Description:
    // - Group headers and the rows inside them look different from plain rows.
    //   The template is shared, so the differences are applied here.
    void CommandHistoryControl::_ContainerContentChanging(const Windows::UI::Xaml::Controls::ListViewBase& /*sender*/, const Windows::UI::Xaml::Controls::ContainerContentChangingEventArgs& args)
    {
        if (args.InRecycleQueue())
        {
            return;
        }

        const auto index = args.ItemIndex();
        if (index >= _rows.size())
        {
            return;
        }

        const auto container = args.ItemContainer();
        if (!container)
        {
            return;
        }

        const auto row = _rows[index];
        const auto grouped = _checked(GroupCheckBox().IsChecked());

        if (const auto text = _findDescendant<Windows::UI::Xaml::Controls::TextBlock>(container))
        {
            if (row.IsGroup)
            {
                text.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                text.Margin(Windows::UI::Xaml::Thickness{ 4.0, 6.0, 4.0, 6.0 });
            }
            else
            {
                const auto indent = grouped ? 18.0 : 4.0;
                text.FontWeight(Windows::UI::Text::FontWeights::Normal());
                text.Margin(Windows::UI::Xaml::Thickness{ indent, 4.0, 4.0, 4.0 });
            }
        }

        if (const auto button = _findDescendant<Windows::UI::Xaml::Controls::Button>(container))
        {
            button.Visibility(row.IsGroup ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible);
        }
    }

    void CommandHistoryControl::_ResizePointerPressed(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs)
    {
        const auto element = sender.try_as<UIElement>();
        if (!element)
        {
            return;
        }

        _resizing = element.CapturePointer(eventArgs.Pointer());
        if (_resizing)
        {
            _resizePointerId = eventArgs.Pointer().PointerId();
            _resizeStartWidth = Width();
            // Window coordinates, so that this widget moving doesn't change the delta.
            _resizeStartX = eventArgs.GetCurrentPoint(nullptr).Position().X;
            eventArgs.Handled(true);
        }
    }

    void CommandHistoryControl::_ResizePointerMoved(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs)
    {
        if (!_resizing || eventArgs.Pointer().PointerId() != _resizePointerId)
        {
            return;
        }

        // The grip is on the left edge, so moving left makes the widget wider.
        const auto x = eventArgs.GetCurrentPoint(nullptr).Position().X;
        Width(std::clamp(_resizeStartWidth - (x - _resizeStartX), MinPanelWidth, MaxPanelWidth));
        eventArgs.Handled(true);
    }

    void CommandHistoryControl::_ResizePointerReleased(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs)
    {
        if (!_resizing || eventArgs.Pointer().PointerId() != _resizePointerId)
        {
            return;
        }

        _resizing = false;
        _resizePointerId = 0;

        if (const auto element = sender.try_as<UIElement>())
        {
            element.ReleasePointerCapture(eventArgs.Pointer());
        }

        _SaveState();
    }

    void CommandHistoryControl::_EntryPointerEntered(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& /*eventArgs*/)
    {
        _SetEntryHover(sender, 1.0);
    }

    void CommandHistoryControl::_EntryPointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& /*eventArgs*/)
    {
        _SetEntryHover(sender, 0.0);
    }

    // Method Description:
    // - Fades the hover background of an entry in or out. The entry template
    //   is a grid whose first child is that background.
    // Arguments:
    // - sender: the entry template's root grid.
    // - opacity: 1 to show the hover background, 0 to hide it.
    // Return Value:
    // - <none>
    void CommandHistoryControl::_SetEntryHover(const Windows::Foundation::IInspectable& sender, const double opacity)
    {
        const auto grid = sender.try_as<Windows::UI::Xaml::Controls::Grid>();
        if (!grid || grid.Children().Size() == 0)
        {
            return;
        }

        if (const auto background = grid.Children().GetAt(0).try_as<Windows::UI::Xaml::UIElement>())
        {
            background.Opacity(opacity);
        }
    }

    void CommandHistoryControl::_RefreshClick(const Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& /*eventArgs*/)
    {
        _Refresh();
    }

    void CommandHistoryControl::_CloseClick(const Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& /*eventArgs*/)
    {
        _SetOpen(false);
    }

    void CommandHistoryControl::_PointerPressed(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs)
    {
        const auto point = eventArgs.GetCurrentPoint(CommandHistoryListView());
        if (point.Properties().IsLeftButtonPressed())
        {
            _pointerTracking = true;
            _pointerId = eventArgs.Pointer().PointerId();
            _pointerStart = point.Position();
        }
        else
        {
            _pointerTracking = false;
        }
    }

    void CommandHistoryControl::_PointerReleased(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs)
    {
        if (!_pointerTracking || eventArgs.Pointer().PointerId() != _pointerId)
        {
            return;
        }
        _pointerTracking = false;

        const auto position = eventArgs.GetCurrentPoint(CommandHistoryListView()).Position();
        const auto dx = position.X - _pointerStart.X;
        const auto dy = position.Y - _pointerStart.Y;
        // If the pointer moved while the button was down, then the user was
        // dragging to select text, not clicking on the entry.
        if (dx * dx + dy * dy > 9.0f)
        {
            return;
        }

        const auto source = eventArgs.OriginalSource().try_as<DependencyObject>();
        if (!source)
        {
            return;
        }

        // Clicks on the row's own buttons are not activations.
        for (auto element = source; element; element = Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(element))
        {
            if (element.try_as<Windows::UI::Xaml::Controls::Primitives::ButtonBase>())
            {
                return;
            }
        }

        Windows::UI::Xaml::Controls::ListViewItem item{ nullptr };
        auto element = source;
        while (element)
        {
            if (const auto candidate = element.try_as<Windows::UI::Xaml::Controls::ListViewItem>())
            {
                item = candidate;
                break;
            }
            element = Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(element);
        }

        if (!item)
        {
            return;
        }

        const auto index = CommandHistoryListView().IndexFromContainer(item);
        if (index < 0 || static_cast<uint32_t>(index) >= _rows.size())
        {
            return;
        }

        const auto& row = _rows[static_cast<size_t>(index)];

        // Clicking the header of a group opens or closes that group.
        if (row.IsGroup)
        {
            if (!_collapsedGroups.insert(row.GroupKey).second)
            {
                _collapsedGroups.erase(row.GroupKey);
            }
            _ForgetInserted();
            _ApplyFilter();
            return;
        }

        const auto command = row.Command;

        // A second click on the entry that the previous click just put on the
        // command line runs that command, instead of putting it there again.
        // This is checked before the selection below, because the second click of
        // a double click selects the word under the pointer.
        const auto now = GetTickCount64();
        const auto window = std::max<uint64_t>(GetDoubleClickTime(), DoubleClickWindow);
        if (command == _lastInsertedCommand && now - _lastInsertedTime <= window)
        {
            _ForgetInserted();
            _Run();
            return;
        }

        if (const auto text = source.try_as<Windows::UI::Xaml::Controls::TextBlock>())
        {
            // A click that selected text is a selection gesture, not an
            // activation.
            if (!text.SelectedText().empty())
            {
                return;
            }
        }

        _lastInsertedCommand = command;
        _lastInsertedTime = now;
        _Activate(command);
    }

    // Method Description:
    // - Runs the command that the first click of a double click put on the
    //   shell's command line, by submitting that line.
    // Return Value:
    // - <none>
    safe_void_coroutine CommandHistoryControl::_Run()
    {
        if (const auto control{ _ActiveTerminal() })
        {
            control.WriteInputString(L"\r", winrt::Microsoft::Terminal::Control::WriteInputStringType::Raw);

            const auto weak = get_weak();
            co_await wil::resume_foreground(Dispatcher());
            if (weak.get())
            {
                control.Focus(Windows::UI::Xaml::FocusState::Programmatic);
            }
        }
    }

    safe_void_coroutine CommandHistoryControl::_Activate(winrt::hstring command)
    {
        if (const auto control{ _ActiveTerminal() })
        {
            // Put the command on the shell's command line, but don't run it.
            control.WriteInputString(command, winrt::Microsoft::Terminal::Control::WriteInputStringType::Raw);

            // Focus the control once the click has been fully handled. The
            // selectable text in the entry takes focus while handling the
            // pointer release, so focusing synchronously would be undone.
            const auto weak = get_weak();
            co_await wil::resume_foreground(Dispatcher());
            if (weak.get())
            {
                control.Focus(Windows::UI::Xaml::FocusState::Programmatic);
            }
        }
    }
}
