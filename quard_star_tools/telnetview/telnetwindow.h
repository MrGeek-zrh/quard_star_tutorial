#ifndef TELNETWINDOW_H
#define TELNETWINDOW_H

#include <QMainWindow>
#include <QPlainTextEdit>

#include "QTelnet.h"

namespace Ui {
class TelnetWindow;
}

class TelnetWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit TelnetWindow(const QString &addr, int port, QWidget *parent = nullptr);
    ~TelnetWindow();
    QString severaddr;
    int severport;
    void reConnect(void);

private slots:
    void addText(const char *msg, int count);
    void refresh_clicked();

protected:
    void keyPressEvent(QKeyEvent *event);
    
private:
    void insertPlainText(QByteArray data);
    Ui::TelnetWindow *ui;
    QTelnet *telnet;
    QByteArray datapool;
};

#endif // TELNETWINDOW_H
