#ifndef REPOSITORYTREEWIDGET_H
#define REPOSITORYTREEWIDGET_H

#include <QTreeWidget>
#include "RepositoryInfo.h"

class MainWindow;
struct RepositoryInfo;

class RepositoryTreeWidget : public QTreeWidget {
	Q_OBJECT
	friend class RepositoryTreeWidgetItemDelegate;
public:
	enum class RepositoryListStyle {
		_Keep,
		Standard,
		SortRecent,
	};
	struct Filter {
		QString text;
		std::shared_ptr<QRegularExpression> re;
		Filter() = default;
		Filter(const QString &text)
			: text(text)
			, re(std::make_shared<QRegularExpression>(text, QRegularExpression::CaseInsensitiveOption))
		{
		}
		bool isEmpty() const
		{
			return text.isEmpty();
		}
	};
private:
	struct Private;
	Private *m;
	MainWindow *mainwindow();
	QTreeWidgetItem *current_item = nullptr;
	Filter filter() const;
	RepositoryListStyle current_repository_list_style_ = RepositoryListStyle::Standard;
protected:
	void paintEvent(QPaintEvent *event) override;
	void dropEvent(QDropEvent *event) override;
	void dragEnterEvent(QDragEnterEvent *event) override;
private:
	enum Type {
		Group,
		Repository,
	};
	static QTreeWidgetItem *newQTreeWidgetItem(const QString &name, Type kind, int index);
	Filter makeFilter(const QString &filtertext);
public:
	static QTreeWidgetItem *newQTreeWidgetGroupItem(QString const &name);
	static QTreeWidgetItem *newQTreeWidgetRepositoryItem(const QString &name, int index);
public:
	explicit RepositoryTreeWidget(QWidget *parent = nullptr);
	~RepositoryTreeWidget();
	void enableDragAndDrop(bool enabled);
	void setFilter(const Filter &filter);
	void setRepositoryListStyle(RepositoryListStyle style);
	RepositoryListStyle currentRepositoryListStyle() const;
	void updateList(RepositoryTreeWidget::RepositoryListStyle style, const QList<RepositoryInfo> &repos, const QString &filtertext, int select_row);
signals:
	void dropped();
};

#endif // REPOSITORYTREEWIDGET_H
