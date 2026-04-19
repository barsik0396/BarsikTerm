#include "MainWindow.h"
#include "TerminalWidget.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("BarsikTerm");
    resize(900, 600);

    auto *terminal = new TerminalWidget(this);
    setCentralWidget(terminal);
}