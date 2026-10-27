#pragma once
namespace kobalt {
class ReadingPage;
namespace kindle {
class Window;
bool checkLibraryInput(Window &window);
bool checkReaderInput(Window &window, ReadingPage &page);
}
}
