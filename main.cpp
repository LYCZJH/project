#include "mainwindow.h"

#include <QApplication>
#ifdef ZZZ
int mainwindowtest();
#endif
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    qRegisterMetaType<int*>("int*");
#ifdef ZZZ
    return mainwindowtest();
#else
    MainWindow w;
    w.show();
    return QApplication::exec();
#endif
}