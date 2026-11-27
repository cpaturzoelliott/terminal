// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <SafeDispatcherTimer.h>

#include <unordered_set>

#include "CommandHistoryControl.g.h"

namespace winrt::TerminalApp::implementation
{
    // A self-contained command history widget. It owns its UI, its refresh
    // timer, and its interaction handling, and it looks up the terminal it
    // should show the history of from the page it lives in.
    struct CommandHistoryControl : CommandHistoryControlT<CommandHistoryControl>
    {
        CommandHistoryControl();

        void Toggle();

        // Bound from XAML.
        void _RefreshClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _CloseClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _SearchTextChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::TextChangedEventArgs& eventArgs);
        void _FilterChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _SortChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _GroupChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _ExpandAllClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _CollapseAllClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _RemoveClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _ContainerContentChanging(const Windows::UI::Xaml::Controls::ListViewBase& sender, const Windows::UI::Xaml::Controls::ContainerContentChangingEventArgs& args);
        void _ResizePointerPressed(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        void _ResizePointerMoved(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        void _ResizePointerReleased(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        void _EntryPointerEntered(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        void _EntryPointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);

    private:
        winrt::Microsoft::Terminal::Control::TermControl _ActiveTerminal() const;
        std::vector<winrt::hstring> _ReadShellHistory(winrt::hstring commandline);
        void _LoadState();
        void _SaveState();
        void _SetOpen(bool open);
        void _ForgetInserted();
        void _Refresh();
        void _ApplyFilter();
        void _SetEntryHover(const Windows::Foundation::IInspectable& sender, double opacity);
        void _RefreshTimerTick(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& args);
        void _PointerPressed(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        void _PointerReleased(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        safe_void_coroutine _Activate(winrt::hstring command);
        safe_void_coroutine _Run();

        // A row of the list: either a command, or the header of a group of
        // commands that start with the same word. _items mirrors these.
        struct HistoryRow
        {
            bool IsGroup{ false };
            winrt::hstring Text;
            winrt::hstring Command;
            std::wstring GroupKey;
        };
        std::vector<HistoryRow> _rows;

        // Commands the user removed from the list, and the groups they closed.
        // Commands are keyed by their dedupe key (lower case, no outer spaces).
        std::unordered_set<std::wstring> _hiddenCommands;
        std::unordered_set<std::wstring> _collapsedGroups;
        // The groups in the current view, so that they can all be collapsed.
        std::unordered_set<std::wstring> _visibleGroups;

        Windows::Foundation::Collections::IObservableVector<winrt::hstring> _items{ nullptr };

        // All of the commands, newest first. _items is the filtered, sorted
        // view of this that the list shows.
        std::vector<winrt::hstring> _commands;

        SafeDispatcherTimer _refreshTimer;

        bool _resizing{ false };
        uint32_t _resizePointerId{ 0 };
        double _resizeStartWidth{ 0 };
        double _resizeStartX{ 0 };

        // Cache for the shell's persisted history file.
        std::wstring _historyPath;
        uint64_t _historyWriteTime{ 0 };
        std::vector<winrt::hstring> _historyCommands;
        bool _open{ false };
        // Set while the state file is being read, so that restoring the checkbox
        // values doesn't write the file back out again.
        bool _loadingState{ false };
        Windows::Foundation::Point _pointerStart{};
        uint32_t _pointerId{ 0 };
        bool _pointerTracking{ false };

        // The entry that the last click put on the command line, and when. A
        // second click on it within the double click interval runs it instead of
        // inserting it again.
        winrt::hstring _lastInsertedCommand;
        uint64_t _lastInsertedTime{ 0 };
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(CommandHistoryControl);
}
