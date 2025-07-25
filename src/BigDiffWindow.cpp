#include <memory>

#include "BigDiffWindow.h"
#include "ui_BigDiffWindow.h"

struct BigDiffWindow::Private {
	TextEditorEnginePtr text_editor_engine;
	FileDiffWidget::InitParam_ param;
};

BigDiffWindow::BigDiffWindow(QWidget *parent)
	: QDialog(parent)
	, m(new Private)
	, ui(new Ui::BigDiffWindow)
{
	ui->setupUi(this);
	Qt::WindowFlags flags = windowFlags();
	flags &= ~Qt::WindowContextHelpButtonHint;
	flags |= Qt::WindowMaximizeButtonHint;
	setWindowFlags(flags);

	ui->widget_diff->setMaximizeButtonEnabled(false);

	connect(ui->widget_diff, &FileDiffWidget::textcodecChanged, [&](){ updateDiffView(); });
}

BigDiffWindow::~BigDiffWindow()
{
	delete m;
	delete ui;
}

void BigDiffWindow::setTextCodec(std::shared_ptr<MyTextCodec> codec)
{
	m->text_editor_engine = std::make_shared<TextEditorEngine>();
	ui->widget_diff->setTextCodec(codec);
}

void BigDiffWindow::updateDiffView()
{
	ui->widget_diff->updateDiffView(m->param.diff, m->param.uncommited);
}

void BigDiffWindow::init(FileDiffWidget::InitParam_ const &param)
{
	ui->widget_diff->init();
	m->param = param;

	{
		QString name = m->param.diff.path;
		int i = name.lastIndexOf('/');
		if (i >= 0) {
			name = name.mid(i + 1);
		}
		ui->lineEdit_center->setText(name);
	}
	auto Text = [](QString id){
		if (id.startsWith(PATH_PREFIX)) {
			id = id.mid(1);
		}
		return id;
	};
	ui->lineEdit_left->setText(Text(m->param.diff.blob.a_id_or_path));
	ui->lineEdit_right->setText(Text(m->param.diff.blob.b_id_or_path));

	switch (m->param.view_style) {
	case FileDiffWidget::ViewStyle::LeftOnly:
		ui->widget_diff->setLeftOnly(m->param.diff, m->param.bytes_a);
		break;
	case FileDiffWidget::ViewStyle::RightOnly:
		ui->widget_diff->setRightOnly(m->param.diff, m->param.bytes_b);
		break;
	case FileDiffWidget::ViewStyle::SideBySideText:
		ui->widget_diff->setSideBySide(m->param.diff, m->param.bytes_a, m->param.uncommited, m->param.workingdir);
		break;
	case FileDiffWidget::ViewStyle::SideBySideImage:
		ui->widget_diff->setSideBySide_(m->param.diff, m->param.bytes_a, m->param.bytes_b, m->param.workingdir);
		break;
	}
}




