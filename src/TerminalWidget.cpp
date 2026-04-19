#include "TerminalWidget.h"

#include <QScrollBar>
#include <QDebug>

#include <pty.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_display = new QPlainTextEdit(this);
    m_display->setReadOnly(true);
    m_display->setFont(QFont("Monospace", 11));
    m_display->setStyleSheet(
        "QPlainTextEdit {"
        "  background-color: #1e1e2e;"
        "  color: #cdd6f4;"
        "  border: none;"
        "  padding: 4px;"
        "}"
    );
    m_display->setMaximumBlockCount(10000);
    layout->addWidget(m_display);

    // дефолтный формат: светлый текст на тёмном фоне
    m_fmt.setForeground(QColor("#cdd6f4"));
    m_fmt.setBackground(QColor("#1e1e2e"));

    setFocusPolicy(Qt::StrongFocus);
    startShell();
}

TerminalWidget::~TerminalWidget()
{
    if (m_masterFd >= 0) ::close(m_masterFd);
    if (m_pid > 0)       ::kill(m_pid, SIGKILL);
}

void TerminalWidget::startShell()
{
    int masterFd, slaveFd;
    if (openpty(&masterFd, &slaveFd, nullptr, nullptr, nullptr) < 0) {
        qWarning() << "openpty failed";
        return;
    }
    m_masterFd = masterFd;

    m_pid = fork();
    if (m_pid == 0) {
        ::close(masterFd);
        setsid();
        ioctl(slaveFd, TIOCSCTTY, 0);
        dup2(slaveFd, STDIN_FILENO);
        dup2(slaveFd, STDOUT_FILENO);
        dup2(slaveFd, STDERR_FILENO);
        ::close(slaveFd);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execl("/bin/bash", "bash", "--login", "-i", nullptr);
        _exit(1);
    }
    ::close(slaveFd);

    fcntl(masterFd, F_SETFL, fcntl(masterFd, F_GETFL, 0) | O_NONBLOCK);

    m_notifier = new QSocketNotifier(masterFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &TerminalWidget::onPtyReadable);

    QTimer::singleShot(100, this, &TerminalWidget::updatePtySize);
}

void TerminalWidget::updatePtySize()
{
    if (m_masterFd < 0) return;
    QFontMetrics fm(m_display->font());
    int cols = qMax(1, m_display->viewport()->width()  / fm.horizontalAdvance('M'));
    int rows = qMax(1, m_display->viewport()->height() / fm.height());
    struct winsize ws { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(m_masterFd, TIOCSWINSZ, &ws);
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePtySize();
}

void TerminalWidget::onPtyReadable()
{
    char buf[4096];
    ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
    if (n > 0)
        processInput(QByteArray(buf, n));
}

void TerminalWidget::processInput(const QByteArray &raw)
{
    // Парсим поток байт вручную
    // Состояния: обычный текст, ESC получен, ESC[ получен (CSI)
    QString text;
    int i = 0;
    while (i < raw.size()) {
        unsigned char c = raw[i];

        if (c == '\033') {
            // сбрасываем накопленный текст
            if (!text.isEmpty()) { appendText(text); text.clear(); }

            if (i + 1 < raw.size() && raw[i+1] == '[') {
                // CSI sequence: ESC [ ... <letter>
                i += 2;
                QByteArray seq;
                while (i < raw.size() && (raw[i] < 0x40 || raw[i] > 0x7e))
                    seq.append(raw[i++]);
                char cmd = (i < raw.size()) ? raw[i++] : 0;

                if (cmd == 'm') {
                    // цветовая последовательность
                    QString s = QString::fromLatin1(seq);
                    QStringList params = s.isEmpty() ? QStringList{"0"} : s.split(';');
                    applyAnsiCodes(params);
                }
                // все остальные CSI команды (курсор, очистка и т.д.) — просто игнорируем
            } else if (i + 1 < raw.size() && raw[i+1] == ']') {
                // OSC sequence: ESC ] ... BEL или ST
                i += 2;
                while (i < raw.size() && raw[i] != '\007' && raw[i] != '\033')
                    i++;
                if (i < raw.size() && raw[i] == '\007') i++;
            } else {
                // другие escape — пропускаем следующий байт
                i += 2;
            }
        } else if (c == '\r') {
            i++;
            // \r без \n — возврат каретки, игнорируем (bash сам пришлёт \n)
        } else if (c == '\b' || c == 127) {
            // backspace от PTY — удаляем последний символ в дисплее
            if (!text.isEmpty()) {
                text.chop(1);
            } else {
                QTextCursor cursor = m_display->textCursor();
                cursor.movePosition(QTextCursor::End);
                cursor.deletePreviousChar();
                m_display->setTextCursor(cursor);
            }
            i++;
        } else if (c == '\007') {
            // BEL — игнорируем
            i++;
        } else {
            // собираем UTF-8 многобайтную последовательность
            int seqLen = 1;
            if      ((c & 0xE0) == 0xC0) seqLen = 2;
            else if ((c & 0xF0) == 0xE0) seqLen = 3;
            else if ((c & 0xF8) == 0xF0) seqLen = 4;

            if (i + seqLen <= raw.size()) {
                text.append(QString::fromUtf8(raw.constData() + i, seqLen));
                i += seqLen;
            } else {
                i++;
            }
        }
    }
    if (!text.isEmpty()) appendText(text);

    m_display->verticalScrollBar()->setValue(m_display->verticalScrollBar()->maximum());
}

void TerminalWidget::appendText(const QString &text)
{
    QTextCursor cursor = m_display->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text, m_fmt);
    m_display->setTextCursor(cursor);
}

void TerminalWidget::applyAnsiCodes(const QStringList &params)
{
    int i = 0;
    while (i < params.size()) {
        int code = params[i].toInt();
        if (code == 0) {
            m_fmt = QTextCharFormat();
            m_fmt.setForeground(QColor("#cdd6f4"));
            m_fmt.setBackground(QColor("#1e1e2e"));
        } else if (code == 1) {
            m_fmt.setFontWeight(QFont::Bold);
        } else if (code == 2) {
            m_fmt.setFontWeight(QFont::Light);
        } else if (code == 3) {
            m_fmt.setFontItalic(true);
        } else if (code == 4) {
            m_fmt.setFontUnderline(true);
        } else if (code == 22) {
            m_fmt.setFontWeight(QFont::Normal);
        } else if (code == 23) {
            m_fmt.setFontItalic(false);
        } else if (code == 24) {
            m_fmt.setFontUnderline(false);
        } else if (code >= 30 && code <= 37) {
            static const QColor fg[] = {
                "#45475a","#f38ba8","#a6e3a1","#f9e2af",
                "#89b4fa","#f5c2e7","#94e2d5","#cdd6f4"
            };
            m_fmt.setForeground(fg[code - 30]);
        } else if (code == 39) {
            m_fmt.setForeground(QColor("#cdd6f4"));
        } else if (code >= 40 && code <= 47) {
            static const QColor bg[] = {
                "#45475a","#f38ba8","#a6e3a1","#f9e2af",
                "#89b4fa","#f5c2e7","#94e2d5","#cdd6f4"
            };
            m_fmt.setBackground(bg[code - 40]);
        } else if (code == 49) {
            m_fmt.setBackground(QColor("#1e1e2e"));
        } else if (code >= 90 && code <= 97) {
            static const QColor bfg[] = {
                "#585b70","#f38ba8","#a6e3a1","#f9e2af",
                "#89b4fa","#f5c2e7","#94e2d5","#ffffff"
            };
            m_fmt.setForeground(bfg[code - 90]);
        } else if (code >= 100 && code <= 107) {
            static const QColor bbg[] = {
                "#585b70","#f38ba8","#a6e3a1","#f9e2af",
                "#89b4fa","#f5c2e7","#94e2d5","#ffffff"
            };
            m_fmt.setBackground(bbg[code - 100]);
        } else if (code == 38 && i + 1 < params.size()) {
            int mode = params[i+1].toInt();
            if (mode == 5 && i + 2 < params.size()) {
                m_fmt.setForeground(ansi256ToColor(params[i+2].toInt()));
                i += 3; continue;
            } else if (mode == 2 && i + 4 < params.size()) {
                m_fmt.setForeground(QColor(params[i+2].toInt(), params[i+3].toInt(), params[i+4].toInt()));
                i += 5; continue;
            }
        } else if (code == 48 && i + 1 < params.size()) {
            int mode = params[i+1].toInt();
            if (mode == 5 && i + 2 < params.size()) {
                m_fmt.setBackground(ansi256ToColor(params[i+2].toInt()));
                i += 3; continue;
            } else if (mode == 2 && i + 4 < params.size()) {
                m_fmt.setBackground(QColor(params[i+2].toInt(), params[i+3].toInt(), params[i+4].toInt()));
                i += 5; continue;
            }
        }
        i++;
    }
}

QColor TerminalWidget::ansi256ToColor(int n)
{
    if (n < 16) {
        static const QColor c[] = {
            "#1e1e2e","#f38ba8","#a6e3a1","#f9e2af",
            "#89b4fa","#f5c2e7","#94e2d5","#cdd6f4",
            "#585b70","#f38ba8","#a6e3a1","#f9e2af",
            "#89b4fa","#f5c2e7","#94e2d5","#ffffff"
        };
        return c[n];
    } else if (n < 232) {
        n -= 16;
        int b = n % 6; n /= 6;
        int g = n % 6; n /= 6;
        int r = n;
        auto v = [](int x) { return x ? 55 + x * 40 : 0; };
        return QColor(v(r), v(g), v(b));
    } else {
        int gray = 8 + (n - 232) * 10;
        return QColor(gray, gray, gray);
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_masterFd < 0) return;

    QByteArray data;

    if (event->modifiers() & Qt::ControlModifier) {
        int key = event->key();
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            data.append(char(key - Qt::Key_A + 1));
        else if (key == Qt::Key_BracketLeft)
            data.append('\x1b');
    } else {
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:    data.append('\r');        break;
        case Qt::Key_Backspace:data.append('\x7f');      break;
        case Qt::Key_Tab:      data.append('\t');        break;
        case Qt::Key_Escape:   data.append('\x1b');      break;
        case Qt::Key_Up:       data.append("\x1b[A");   break;
        case Qt::Key_Down:     data.append("\x1b[B");   break;
        case Qt::Key_Right:    data.append("\x1b[C");   break;
        case Qt::Key_Left:     data.append("\x1b[D");   break;
        case Qt::Key_Home:     data.append("\x1b[H");   break;
        case Qt::Key_End:      data.append("\x1b[F");   break;
        case Qt::Key_Delete:   data.append("\x1b[3~");  break;
        default:               data = event->text().toUtf8(); break;
        }
    }

    if (!data.isEmpty())
        ::write(m_masterFd, data.constData(), data.size());
}