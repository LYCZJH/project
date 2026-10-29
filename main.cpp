#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    qRegisterMetaType<int*>("int*");
    MainWindow w;
    w.show();
    return QApplication::exec();
}
