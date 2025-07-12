#include "GenerateCommitMessageDialog.h"
#include "ui_GenerateCommitMessageDialog.h"
#include "ApplicationGlobal.h"
#include "CommitMessageGenerator.h"
#include "MainWindow.h"
#include "GenerateCommitMessageThread.h"
#include <QMessageBox>

struct GenerateCommitMessageDialog::Private {
	std::string diff;
	GenerateCommitMessageThread generator;
	QStringList checked_items;
};

GenerateCommitMessageDialog::GenerateCommitMessageDialog(QWidget *parent, const std::string &model_name)
	: QDialog(parent)
	, ui(new Ui::GenerateCommitMessageDialog)
	, m(new Private)
{
	ui->setupUi(this);
	ui->label->setText(QString::fromStdString(model_name));
	
	m->generator.start();
	
	connect(&m->generator, &GenerateCommitMessageThread::ready, this, &GenerateCommitMessageDialog::onReady);
	
}

GenerateCommitMessageDialog::~GenerateCommitMessageDialog()
{
	m->generator.stop();
	delete ui;
	delete m;
}

void GenerateCommitMessageDialog::generate(std::string const &diff)
{
	m->diff = diff;
	
	GlobalSetOverrideWaitCursor();

	m->checked_items = message();

	ui->listWidget->clear();

	ui->listWidget->addItems(m->checked_items);
	for (int i = 0; i < ui->listWidget->count(); i++) {
		auto *item = ui->listWidget->item(i);
		if (m->checked_items.contains(item->text())) {
			item->setCheckState(Qt::Checked);
		}
	}

	ui->pushButton_regenerate->setEnabled(false);
	
	m->generator.request(diff);
}

std::string GenerateCommitMessageDialog::diffText() const
{
	return m->diff;
}

QStringList GenerateCommitMessageDialog::message() const
{
	QStringList list;
	int n = ui->listWidget->count();
	for (int i = 0; i < n; i++) {
		auto *item = ui->listWidget->item(i);
		if (item->checkState() == Qt::Checked) {
			list.append(item->text());
		}
	}
	return list;
}

void GenerateCommitMessageDialog::on_pushButton_regenerate_clicked()
{
	std::string diff = CommitMessageGenerator::diff_head(global->mainwindow->git());
	generate(diff);
}

void GenerateCommitMessageDialog::onReady(const GeneratedCommitMessage &result)
{
	GlobalRestoreOverrideCursor();

	if (result) {
		auto MakeStringList = [](std::vector<std::string> const &list) {
			QStringList ret;
			for (auto const &s : list) {
				ret.append(QString::fromStdString(s));
			}
			return ret;
		};
		int i = ui->listWidget->count();
		ui->listWidget->addItems(MakeStringList(result.messages()));
		int n = ui->listWidget->count();;
		while (i < n) {
			ui->listWidget->item(i)->setCheckState(Qt::Unchecked);
			i++;
		}
		ui->listWidget->setCurrentRow(0);
	} else {
		std::string text = result.error_status() + "\n\n" + result.error_message();
		QMessageBox::warning(this, tr("Failed to generate commit message."), QString::fromStdString(text));
	}
	
	ui->pushButton_regenerate->setEnabled(true);	
}

void GenerateCommitMessageDialog::on_listWidget_itemDoubleClicked(QListWidgetItem *item)
{
	item->setCheckState(Qt::Checked);
	done(QDialog::Accepted);
}

void GenerateCommitMessageDialog::done(int stat)
{
	if (stat == QDialog::Accepted) {
		QStringList list = message();
		if (list.empty()) {
			auto *item = ui->listWidget->currentItem();
			if (item) {
				item->setCheckState(Qt::Checked);
			}
		}
	}
	QDialog::done(stat);
}

