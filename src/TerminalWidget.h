#pragma once

#include <QWidget>
#include <QPlainTextEdit>
#include <QSocketNotifier>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QFont>
#include <QTextCharFormat>
#include <QTimer>
#include <QColor>

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onPtyReadable();

private:
    QPlainTextEdit   *m_display;
    QSocketNotifier  *m_notifier = nullptr;
    int               m_masterFd = -1;
    pid_t             m_pid = -1;

    QTextCharFormat   m_fmt;

    void startShell();
    void updatePtySize();
    void processInput(const QByteArray &raw);
    void appendText(const QString &text);
    void applyAnsiCodes(const QStringList &params);
    QColor ansi256ToColor(int n);
};