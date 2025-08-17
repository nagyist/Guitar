#ifndef COMMITEXPLOREWINDOW_H
#define COMMITEXPLOREWINDOW_H

#include <QDialog>
#include "FileDiffWidget.h"

namespace Ui {
class CommitExploreWindow;
}

class QTreeWidgetItem;
class QListWidgetItem;
class GitObjectCache;

class CommitExploreWindow : public QDialog {
	Q_OBJECT
private:
	Ui::CommitExploreWindow *ui;

	struct Private;
	Private *m;

	void loadTree(GitRunner g, QString const &tree_id);
	void doTreeItemChanged_(GitRunner g, QTreeWidgetItem *current);
	void expandTreeItem_(GitRunner g, QTreeWidgetItem *item);
	MainWindow *mainwindow();
        GitRunner git();
public:
	explicit CommitExploreWindow(QWidget *parent, GitObjectCache *objcache, GitCommitItem const *commit);
	~CommitExploreWindow() override;

	void clearContent();
private slots:
	void on_treeWidget_itemExpanded(QTreeWidgetItem *item);
	void on_treeWidget_currentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
	void on_listWidget_itemDoubleClicked(QListWidgetItem *item);
	void on_listWidget_currentItemChanged(QListWidgetItem *current, QListWidgetItem *previous);
	void on_verticalScrollBar_valueChanged(int);
	void on_horizontalScrollBar_valueChanged(int);
	void on_listWidget_customContextMenuRequested(const QPoint &pos);
};

#endif // COMMITEXPLOREWINDOW_H
