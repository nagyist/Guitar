#ifndef DIALOGHEADERFRAME_H
#define DIALOGHEADERFRAME_H

#include <QFrame>

class DialogHeaderFrame : public QFrame
{
	Q_OBJECT
public:
	explicit DialogHeaderFrame(QWidget *parent = nullptr);

signals:

public slots:

	// QWidget interface
protected:
	void paintEvent(QPaintEvent *);
};

#endif // DIALOGHEADERFRAME_H
