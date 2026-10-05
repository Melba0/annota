// Annota - ide_gui.cpp : the graphical IDE (`annota studio`).
//
// A single window that ties the whole toolchain together:
//   * editor with line numbers, Annota syntax highlighting and diagnostic squiggles
//   * problems panel fed by the analysis layer (click a row to jump to the line)
//   * structure panel showing every function with its ✓ / ? / ✗ contract status
//   * output panel that runs the buffer with F5
//   * hover panel, quick fixes, coverage, and the annotation / check reference dialogs
#include "value.hpp"
#include "commands.hpp"
#include "common.hpp"

#ifdef ANNOTA_QT

#include "analyzer.hpp"
#include "builtins.hpp"
#include "compiler.hpp"
#include "gui.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "vm.hpp"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>

namespace annota {
namespace {

// ---------------------------------------------------------------- module loading
struct IdeLoader : ModuleLoader {
    std::string baseDir;
    std::vector<std::string> dirs;
    std::vector<std::string> loaded;
    explicit IdeLoader(std::string dir) : baseDir(std::move(dir)) {
        dirs = {baseDir, baseDir + "/lib", "lib", "."};
    }
    void remember(const std::string& file) {
        std::string d = file;
        size_t q = d.find_last_of("/\\");
        if (q != std::string::npos) {
            d = d.substr(0, q);
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
        }
    }
    static bool exists(const std::string& p) {
        std::ifstream in(p, std::ios::binary);
        return (bool)in;
    }
    bool loadModule(const std::string& spec, std::vector<Token>& toks, std::string& file) override {
        std::vector<std::string> cands;
        bool pathLike = spec.find('/') != std::string::npos || spec.find('\\') != std::string::npos ||
                        spec.find(".mod") != std::string::npos;
        if (pathLike) {
            cands.push_back(spec);
            for (auto& d : dirs) cands.push_back(d + "/" + spec);
        } else {
            for (auto& d : dirs) cands.push_back(d + "/" + spec + ".mod");
            cands.push_back(spec + ".mod");
        }
        for (auto& c : cands) {
            if (!exists(c)) continue;
            std::ifstream in(c, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            file = c;
            remember(c);
            loaded.push_back(c);
            toks = lex(ss.str(), c);
            return true;
        }
        return false;
    }
};

std::string dirOf(const std::string& path) {
    size_t q = path.find_last_of("/\\");
    return q == std::string::npos ? std::string(".") : path.substr(0, q);
}

// ---------------------------------------------------------------- running a buffer
struct RunOutcome {
    bool ok = false;
    std::string output;
    std::string error;
    int errorLine = 0;
    long long elapsedMs = 0;
    // kept alive so that the IDE can open the program's own `view` windows afterwards
    std::shared_ptr<VM> vm;
    std::vector<std::string> views;
};

RunOutcome runBuffer(const std::string& source, const std::string& path, bool contracts) {
    RunOutcome out;
    long long t0 = clock();
    try {
        IdeLoader loader(dirOf(path));
        std::vector<Token> toks = lex(source, path);
        MacroRegistry registry;
        Parser parser(std::move(toks), path, &loader, &registry);
        Program program = parser.parse();
        Compiler compiler(program, path, contracts);
        CompileResult compiled = compiler.compile();
        auto vm = std::make_shared<VM>();
        registerBuiltins(*vm);
        vm->contracts = contracts;
        vm->capture = true;
        guiInstallInput(*vm);          // `input` asks with a dialog instead of reading stdin
        vm->mainChunk = compiled.main;
        vm->run();
        out.vm = vm;
        out.views = compiled.views;
        out.output = vm->stdoutText;
        out.ok = true;
    } catch (CompileError& e) {
        out.error = e.message;
        out.errorLine = e.line;
    } catch (VMError& e) {
        out.error = e.message;
    } catch (std::exception& e) {
        out.error = e.what();
    }
    out.elapsedMs = (clock() - t0) * 1000LL / CLOCKS_PER_SEC;
    return out;
}

// ---------------------------------------------------------------- editor plumbing
class Editor;

// the line number gutter: painting is delegated to the editor because the Qt text layout
// helpers it needs are protected members of QPlainTextEdit
class LineNumbers : public QWidget {
public:
    explicit LineNumbers(Editor* editor);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    Editor* ed_;
};

class Editor : public QPlainTextEdit {
public:
    explicit Editor(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setMouseTracking(true);
        gutter_ = new LineNumbers(this);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int) { updateGutter(); });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& r, int dy) {
            if (dy) gutter_->scroll(0, dy);
            else gutter_->update(0, r.y(), gutter_->width(), r.height());
        });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { highlightCurrentLine(); });
        updateGutter();
    }

    // ---- used by the gutter
    QSize gutterSizeHint() const {
        int digits = 2;
        int max = std::max(1, blockCount());
        while (max >= 100) { max /= 10; digits++; }
        return QSize(fontMetrics().horizontalAdvance(QLatin1Char('9')) * (digits + 2), 0);
    }
    int gutterWidth() const { return gutter_->sizeHint().width(); }
    void paintGutter(QPaintEvent* ev) {
        QPainter p(gutter_);
        p.fillRect(ev->rect(), QColor(0xF4, 0xF4, 0xF7));
        QTextBlock block = firstVisibleBlock();
        int num = block.blockNumber();
        qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
        qreal bottom = top + blockBoundingRect(block).height();
        while (block.isValid() && top <= ev->rect().bottom()) {
            if (block.isVisible() && bottom >= ev->rect().top()) {
                bool cur = (block.blockNumber() == textCursor().blockNumber());
                p.setPen(cur ? QColor(0x1F, 0x5F, 0xC0) : QColor(0x8A, 0x8A, 0x96));
                p.drawText(0, (int)top, gutter_->width() - 6, fontMetrics().height(), Qt::AlignRight,
                           QString::number(num + 1));
            }
            block = block.next();
            top = bottom;
            bottom = top + blockBoundingRect(block).height();
            num++;
        }
    }

    void setDiagnostics(const std::vector<Diagnostic>& diags) {
        marks_.clear();
        for (auto& d : diags) {
            if (d.suppressed) continue;
            QTextEdit::ExtraSelection sel;
            sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            sel.format.setUnderlineColor(d.severity == Severity::Error
                                             ? QColor(0xD0, 0x30, 0x30)
                                             : (d.severity == Severity::Warning ? QColor(0xD0, 0x90, 0x00)
                                                                                : QColor(0x40, 0x80, 0xC0)));
            QTextCursor cur(document()->findBlockByNumber(std::max(0, d.line - 1)));
            cur.select(QTextCursor::LineUnderCursor);
            sel.cursor = cur;
            marks_.push_back(sel);
        }
        highlightCurrentLine();
    }

    void goToLine(int line) {
        QTextBlock b = document()->findBlockByNumber(std::max(0, line - 1));
        QTextCursor cur(b);
        setTextCursor(cur);
        centerCursor();
        setFocus();
    }

protected:
    void resizeEvent(QResizeEvent* ev) override {
        QPlainTextEdit::resizeEvent(ev);
        QRect cr = contentsRect();
        gutter_->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
    }

    void keyPressEvent(QKeyEvent* ev) override {
        // auto indent, auto closing brackets, tab inserts spaces
        if (ev->key() == Qt::Key_Return) {
            QTextCursor cur = textCursor();
            QString line = cur.block().text();
            QString indent;
            for (QChar c : line) {
                if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) indent += c;
                else break;
            }
            QString extra;
            if (!line.trimmed().isEmpty() && line.trimmed().endsWith(QLatin1Char('(')))
                extra = QStringLiteral("    ");
            QPlainTextEdit::keyPressEvent(ev);
            if (!indent.isEmpty() || !extra.isEmpty()) insertPlainText(indent + extra);
            return;
        }
        if (ev->key() == Qt::Key_Tab && !(ev->modifiers() & Qt::ShiftModifier)) {
            insertPlainText(QStringLiteral("    "));
            return;
        }
        if (ev->text() == QLatin1String("(")) { insertPlainText(QStringLiteral("()")); moveCursor(QTextCursor::Left); return; }
        if (ev->text() == QLatin1String("[")) { insertPlainText(QStringLiteral("[]")); moveCursor(QTextCursor::Left); return; }
        QPlainTextEdit::keyPressEvent(ev);
    }

private:
    void updateGutter() { setViewportMargins(gutterWidth(), 0, 0, 0); }
    void highlightCurrentLine() {
        if (inHighlight_) return;             // setExtraSelections() triggers update requests
        inHighlight_ = true;
        QList<QTextEdit::ExtraSelection> sels = marks_;
        QTextEdit::ExtraSelection line;
        line.format.setBackground(QColor(0xF2, 0xF6, 0xFC));
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = textCursor();
        line.cursor.clearSelection();
        sels.push_back(line);
        setExtraSelections(sels);
        inHighlight_ = false;
    }
    LineNumbers* gutter_;
    QList<QTextEdit::ExtraSelection> marks_;
    bool inHighlight_ = false;
};

LineNumbers::LineNumbers(Editor* editor) : QWidget(editor), ed_(editor) {}
QSize LineNumbers::sizeHint() const { return ed_->gutterSizeHint(); }
void LineNumbers::paintEvent(QPaintEvent* ev) { ed_->paintGutter(ev); }

// ---------------------------------------------------------------- highlighting
class Highlighter : public QSyntaxHighlighter {
public:
    explicit Highlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) {
        auto add = [&](const QString& pattern, const QTextCharFormat& fmt) {
            Rule r;
            r.re = QRegularExpression(pattern);
            r.fmt = fmt;
            rules_.push_back(r);
        };
        QTextCharFormat kw;
        kw.setForeground(QColor(0x00, 0x5C, 0xC5));
        kw.setFontWeight(QFont::Bold);
        for (const char* k : {"new", "del", "const", "if", "elif", "else", "while", "for", "in", "to", "step",
                              "break", "continue", "throw", "except", "print", "input", "use", "macro",
                              "view", "state", "true", "false", "null"})
            add(QStringLiteral("\\b%1\\b").arg(k), kw);

        QTextCharFormat type;
        type.setForeground(QColor(0x7A, 0x3E, 0x9D));
        // the type names come from the same registry the parser and the analyzer use
        // (`builtinTypes()`), so `long`, `longlong`, `int8`, `double`, ... highlight too
        QString typePattern;
        for (auto& t : builtinTypes()) {
            if (!typePattern.isEmpty()) typePattern += "|";
            typePattern += QString::fromUtf8(t.name);
        }
        typePattern += "|Fn|Any|str";
        add(QStringLiteral("\\b(%1)\\b").arg(typePattern), type);

        QTextCharFormat ann;
        ann.setForeground(QColor(0xB0, 0x50, 0x00));
        ann.setFontWeight(QFont::Bold);
        add(QStringLiteral("\\[\\[[A-Za-z_][A-Za-z0-9_]*"), ann);
        add(QStringLiteral("\\]\\]"), ann);

        QTextCharFormat str;
        str.setForeground(QColor(0x0A, 0x7D, 0x2E));
        add(QStringLiteral("\"[^\"\\\\]*(\\\\.[^\"\\\\]*)*\""), str);

        QTextCharFormat num;
        num.setForeground(QColor(0xB0, 0x30, 0x60));
        add(QStringLiteral("\\b[0-9]+(\\.[0-9]+)?\\b"), num);

        QTextCharFormat comment;
        comment.setForeground(QColor(0x80, 0x88, 0x90));
        comment.setFontItalic(true);
        add(QStringLiteral("--[^\n]*"), comment);
        commentFmt_ = comment;
    }

protected:
    void highlightBlock(const QString& text) override {
        for (auto& r : rules_) {
            auto it = r.re.globalMatch(text);
            while (it.hasNext()) {
                auto m = it.next();
                setFormat(m.capturedStart(), m.capturedLength(), r.fmt);
            }
        }
        // -[ ... ]- block comments can span lines
        setCurrentBlockState(0);
        int start = 0;
        if (previousBlockState() != 1) start = text.indexOf(QStringLiteral("-["));
        while (start >= 0) {
            int end = text.indexOf(QStringLiteral("]-"), start + 2);
            int len = end < 0 ? text.length() - start : end - start + 2;
            setFormat(start, len, commentFmt_);
            if (end < 0) { setCurrentBlockState(1); break; }
            start = text.indexOf(QStringLiteral("-["), start + len);
        }
    }

private:
    struct Rule {
        QRegularExpression re;
        QTextCharFormat fmt;
    };
    std::vector<Rule> rules_;
    QTextCharFormat commentFmt_;
};

// ---------------------------------------------------------------- help dialogs
QString annotationsHtml() {
    QString html = QStringLiteral("<h2>标注手册</h2><p>共 %1 条，可用 <code>[[名字: 参数]]</code> 写在语句、函数、循环或文件之前。</p>")
                       .arg(annotationRegistry().size());
    std::string lastCat;
    for (auto& a : annotationRegistry()) {
        if (a.category != lastCat) {
            if (!lastCat.empty()) html += QStringLiteral("</table>");
            html += QStringLiteral("<h3>%1</h3><table width='100%' cellpadding='3'>").arg(QString::fromStdString(a.category));
            lastCat = a.category;
        }
        QString scope;
        for (auto& s : a.scopes) scope += (scope.isEmpty() ? "" : " | ") + QString::fromStdString(s);
        html += QStringLiteral("<tr><td valign='top'><b>[[%1]]</b><br><span style='color:#777'>%2 个参数%3</span></td>"
                               "<td valign='top'>%4<br><span style='color:#555'>%5</span></td></tr>")
                    .arg(QString::fromStdString(a.name))
                    .arg(a.minArgs == a.maxArgs ? QString::number(a.minArgs)
                                                : QStringLiteral("%1..%2").arg(a.minArgs).arg(a.maxArgs))
                    .arg(a.takesKwargs ? QStringLiteral(" +键值") : QString())
                    .arg(QString::fromStdString(a.summary))
                    .arg(QString::fromStdString(a.detail));
        Q_UNUSED(scope);
    }
    html += QStringLiteral("</table>");
    return html;
}

QString checksHtml() {
    QString html = QStringLiteral("<h2>检查手册</h2><p>共 %1 个诊断码；在 <code>[[ignore: 码]]</code> 里引用可以只抑制一条检查。</p>"
                                  "<table width='100%' cellpadding='3'>")
                       .arg(checkCodeRegistry().size());
    for (auto& c : checkCodeRegistry()) {
        html += QStringLiteral("<tr><td valign='top'><code>%1</code></td><td>%2</td></tr>")
                    .arg(QString::fromStdString(c))
                    .arg(QString::fromUtf8(checkCodeSummary(c)));
    }
    html += QStringLiteral("</table>");
    return html;
}

void showHtmlDialog(QWidget* parent, const QString& title, const QString& html) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.resize(760, 560);
    auto* view = new QTextBrowser(&dlg);
    view->setHtml(html);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(view);
    layout->addWidget(buttons);
    dlg.exec();
}

// ---------------------------------------------------------------- main window
class Studio : public QMainWindow {
public:
    Studio(QString path, QString tab) : path_(std::move(path)) {
        buildUi();
        if (!path_.isEmpty()) loadFile(path_);
        else setSource(QStringLiteral("-[ 新文件：按 F5 运行，F6 分析 ]-\n\nprint \"hello\"\n"));
        if (tab == QLatin1String("structure")) rightTabs_->setCurrentIndex(1);
        else if (tab == QLatin1String("coverage")) rightTabs_->setCurrentIndex(2);
        scheduleAnalysis(1);
    }

protected:
    void closeEvent(QCloseEvent* ev) override {
        if (maybeSave()) ev->accept();
        else ev->ignore();
    }

public:
    // used by `annota studio <file> --run|--preview` (headless verification / screenshots)
    void runForTest(bool echo = false) { runCurrent(); if (echo) echoOutput(); }
    void previewForTest(bool echo = false) { runCurrent(); if (echo) echoOutput(); previewView(); }
    void analyzeForTest() { analyzeNow(3); }
    // --echo prints what the output pane holds, so CI can inspect a headless run
    void echoOutput() { std::fputs(output_->toPlainText().toStdString().c_str(), stdout); std::fflush(stdout); }

private:
    // ---- construction
    void buildUi() {
        setWindowTitle(QStringLiteral("Annota Studio"));
        resize(1180, 780);

        editor_ = new Editor(this);
        highlighter_ = new Highlighter(editor_->document());
        connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
            rev_++;
            if (loading_) return;
            dirty_ = true;
            updateTitle();
            scheduleAnalysis(1);
        });
        connect(editor_, &QPlainTextEdit::cursorPositionChanged, this, [this] { updateHover(); });

        problems_ = new QTableWidget(this);
        problems_->setColumnCount(4);
        problems_->setHorizontalHeaderLabels({QStringLiteral("级别"), QStringLiteral("行"), QStringLiteral("检查"), QStringLiteral("说明")});
        problems_->horizontalHeader()->setStretchLastSection(true);
        problems_->verticalHeader()->setVisible(false);
        problems_->setSelectionBehavior(QAbstractItemView::SelectRows);
        problems_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        problems_->setColumnWidth(0, 60);
        problems_->setColumnWidth(1, 50);
        problems_->setColumnWidth(2, 170);
        connect(problems_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            if (row >= 0 && row < (int)diagnostics_.size()) editor_->goToLine(diagnostics_[(size_t)row].line);
        });

        structure_ = new QTreeWidget(this);
        structure_->setColumnCount(2);
        structure_->setHeaderLabels({QStringLiteral("函数"), QStringLiteral("契约")});
        structure_->setRootIsDecorated(false);
        structure_->header()->setStretchLastSection(true);
        connect(structure_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
            editor_->goToLine(it->data(0, Qt::UserRole).toInt());
        });

        rightTabs_ = new QTabWidget(this);
        rightTabs_->addTab(problems_, QStringLiteral("问题"));
        rightTabs_->addTab(structure_, QStringLiteral("结构"));
        coverage_ = new QTextBrowser(this);
        rightTabs_->addTab(coverage_, QStringLiteral("覆盖率"));

        auto* topSplit = new QSplitter(Qt::Horizontal, this);
        topSplit->addWidget(editor_);
        topSplit->addWidget(rightTabs_);
        topSplit->setStretchFactor(0, 3);
        topSplit->setStretchFactor(1, 2);

        output_ = new QPlainTextEdit(this);
        output_->setReadOnly(true);
        output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        output_->setPlaceholderText(QStringLiteral("F5 运行当前缓冲区，输出显示在这里"));
        split_ = new QSplitter(Qt::Vertical, this);
        split_->addWidget(topSplit);
        split_->addWidget(output_);
        split_->setStretchFactor(0, 4);
        split_->setStretchFactor(1, 1);
        setCentralWidget(split_);

        hover_ = new QLabel(this);
        hover_->setWordWrap(true);
        hover_->setMinimumHeight(46);
        hover_->setStyleSheet(QStringLiteral("QLabel { background:#FAFAFC; border-top:1px solid #DDD; padding:6px; }"));
        auto* dock = new QDockWidget(QStringLiteral("悬停 / 状态"), this);
        dock->setWidget(hover_);
        dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        addDockWidget(Qt::BottomDockWidgetArea, dock);

        statusLeft_ = new QLabel(this);
        statusRight_ = new QLabel(this);
        statusBar()->addWidget(statusLeft_);
        statusBar()->addPermanentWidget(statusRight_);

        buildMenus();
        updateTitle();
        updateHover();
    }

    QAction* act(const QString& text, const QKeySequence& key, std::function<void()> fn, QMenu* menu) {
        QAction* a = new QAction(text, this);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        if (menu) menu->addAction(a);
        else addAction(a);
        return a;
    }

    void buildMenus() {
        QMenu* file = menuBar()->addMenu(QStringLiteral("文件(&F)"));
        act(QStringLiteral("新建"), QKeySequence::New, [this] { newFile(); }, file);
        act(QStringLiteral("打开..."), QKeySequence::Open, [this] { openFile(); }, file);
        act(QStringLiteral("保存"), QKeySequence::Save, [this] { saveFile(); }, file);
        act(QStringLiteral("另存为..."), QKeySequence::SaveAs, [this] { saveFileAs(); }, file);
        file->addSeparator();
        act(QStringLiteral("打开示例..."), QKeySequence(), [this] { openExample(); }, file);
        file->addSeparator();
        act(QStringLiteral("退出"), QKeySequence::Quit, [this] { close(); }, file);

        QMenu* edit = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
        act(QStringLiteral("撤销"), QKeySequence::Undo, [this] { editor_->undo(); }, edit);
        act(QStringLiteral("重做"), QKeySequence::Redo, [this] { editor_->redo(); }, edit);
        edit->addSeparator();
        act(QStringLiteral("注释 / 取消注释"), QKeySequence(QStringLiteral("Ctrl+/")), [this] { toggleComment(); }, edit);
        act(QStringLiteral("增加缩进"), QKeySequence(QStringLiteral("Ctrl+]")), [this] { indent(1); }, edit);
        act(QStringLiteral("减少缩进"), QKeySequence(QStringLiteral("Ctrl+[")), [this] { indent(-1); }, edit);

        QMenu* run = menuBar()->addMenu(QStringLiteral("运行(&R)"));
        act(QStringLiteral("运行 (F5)"), QKeySequence(QStringLiteral("F5")), [this] { runCurrent(); }, run);
        act(QStringLiteral("运行并检查契约"), QKeySequence(QStringLiteral("Ctrl+F5")), [this] { runCurrent(true); }, run);
        run->addSeparator();
        act(QStringLiteral("预览界面（打开 view 窗口）"), QKeySequence(QStringLiteral("F7")),
            [this] { previewView(); }, run);
        act(QStringLiteral("打印组件树"), QKeySequence(QStringLiteral("Ctrl+F7")),
            [this] { dumpViewTree(); }, run);
        run->addSeparator();
        act(QStringLiteral("分析 (F6)"), QKeySequence(QStringLiteral("F6")), [this] { analyzeNow(3); }, run);
        act(QStringLiteral("清空输出"), QKeySequence(), [this] { output_->clear(); }, run);

        QMenu* fix = menuBar()->addMenu(QStringLiteral("修复(&X)"));
        act(QStringLiteral("把修复插入到选中问题所在行"), QKeySequence(QStringLiteral("Ctrl+1")),
            [this] { applyFix(problems_->currentRow()); }, fix);
        act(QStringLiteral("全部可自动插入的修复"), QKeySequence(QStringLiteral("Ctrl+Shift+1")),
            [this] { applyAllFixes(); }, fix);

        QMenu* help = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
        act(QStringLiteral("标注手册"), QKeySequence(QStringLiteral("F1")),
            [this] { showHtmlDialog(this, QStringLiteral("标注手册"), annotationsHtml()); }, help);
        act(QStringLiteral("检查手册"), QKeySequence(QStringLiteral("F2")),
            [this] { showHtmlDialog(this, QStringLiteral("检查手册"), checksHtml()); }, help);
        act(QStringLiteral("文件 API 手册"), QKeySequence(), [this] { showFileApi(); }, help);
        act(QStringLiteral("快捷键"), QKeySequence(), [this] { showShortcuts(); }, help);
        act(QStringLiteral("关于"), QKeySequence(), [this] { showAbout(); }, help);

        QToolBar* bar = addToolBar(QStringLiteral("主工具栏"));
        bar->setMovable(false);
        bar->addAction(act(QStringLiteral("运行"), QKeySequence(), [this] { runCurrent(); }, nullptr));
        bar->addAction(act(QStringLiteral("预览界面"), QKeySequence(), [this] { previewView(); }, nullptr));
        bar->addAction(act(QStringLiteral("分析"), QKeySequence(), [this] { analyzeNow(3); }, nullptr));
        bar->addAction(act(QStringLiteral("保存"), QKeySequence(), [this] { saveFile(); }, nullptr));
        bar->addSeparator();
        bar->addAction(act(QStringLiteral("标注手册"), QKeySequence(), [this] { showHtmlDialog(this, QStringLiteral("标注手册"), annotationsHtml()); }, nullptr));
        bar->addAction(act(QStringLiteral("检查手册"), QKeySequence(), [this] { showHtmlDialog(this, QStringLiteral("检查手册"), checksHtml()); }, nullptr));
        bar->addAction(act(QStringLiteral("文件 API"), QKeySequence(), [this] { showFileApi(); }, nullptr));
    }

    // ---- files
    void setSource(const QString& text) {
        loading_ = true;
        editor_->setPlainText(text);
        loading_ = false;
        dirty_ = false;
        updateTitle();
    }

    void loadFile(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, QStringLiteral("打开失败"), QStringLiteral("无法读取 %1").arg(path));
            return;
        }
        setSource(QString::fromUtf8(f.readAll()));
        path_ = path;
        updateTitle();
        analyzeNow(3);
    }

    bool maybeSave() {        if (!dirty_) return true;
        auto answer = QMessageBox::question(this, QStringLiteral("未保存"),
                                            QStringLiteral("当前文件有未保存的修改，是否保存？"),
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Save) return saveFile();
        return answer == QMessageBox::Discard;
    }

    bool saveFile() {
        if (path_.isEmpty()) return saveFileAs();
        QFile f(path_);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法写入 %1").arg(path_));
            return false;
        }
        f.write(editor_->toPlainText().toUtf8());
        f.close();
        dirty_ = false;
        updateTitle();
        analyzeNow(3);
        statusBar()->showMessage(QStringLiteral("已保存 %1").arg(path_), 4000);
        return true;
    }

    bool saveFileAs() {
        QString path = QFileDialog::getSaveFileName(this, QStringLiteral("另存为"), path_,
                                                   QStringLiteral("Annota 源文件 (*.ant);;所有文件 (*)"));
        if (path.isEmpty()) return false;
        path_ = path;
        return saveFile();
    }

    void newFile() {
        if (!maybeSave()) return;
        path_.clear();
        setSource(QStringLiteral("-[ 新文件 ]-\n\n"));
        diagnostics_.clear();
        updateProblems();
    }

    void openFile() {
        if (!maybeSave()) return;
        QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开"), path_,
                                                   QStringLiteral("Annota 源文件 (*.ant *.mod);;所有文件 (*)"));
        if (path.isEmpty()) return;
        loadFile(path);
    }

    void openExample() {
        QString dir = QFileInfo(path_.isEmpty() ? QStringLiteral("examples") : path_).absolutePath();
        QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开示例"), dir,
                                                   QStringLiteral("Annota 源文件 (*.ant *.mod)"));
        if (!path.isEmpty()) loadFile(path);
    }

    void updateTitle() {
        QString name = path_.isEmpty() ? QStringLiteral("未命名") : QFileInfo(path_).fileName();
        setWindowTitle(QStringLiteral("Annota Studio — %1%2").arg(name, dirty_ ? QStringLiteral(" *") : QString()));
    }

    // ---- editing helpers
    void toggleComment() {
        QTextCursor cur = editor_->textCursor();
        int from = cur.selectionStart();
        int to = cur.selectionEnd();
        QTextCursor c(editor_->document());
        c.setPosition(from);
        int firstLine = c.blockNumber();
        c.setPosition(to);
        int lastLine = c.blockNumber();
        c.beginEditBlock();
        for (int i = firstLine; i <= lastLine; i++) {
            QTextBlock b = editor_->document()->findBlockByNumber(i);
            QString text = b.text();
            QTextCursor lc(b);
            if (text.trimmed().startsWith(QStringLiteral("--"))) {
                int idx = text.indexOf(QStringLiteral("--"));
                lc.setPosition(b.position() + idx);
                lc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 3);
                lc.removeSelectedText();
            } else {
                lc.setPosition(b.position());
                lc.insertText(QStringLiteral("-- "));
            }
        }
        c.endEditBlock();
    }

    void indent(int dir) {
        QTextCursor cur = editor_->textCursor();
        QTextCursor c(editor_->document());
        c.setPosition(cur.selectionStart());
        int firstLine = c.blockNumber();
        c.setPosition(cur.selectionEnd());
        int lastLine = c.blockNumber();
        c.beginEditBlock();
        for (int i = firstLine; i <= lastLine; i++) {
            QTextBlock b = editor_->document()->findBlockByNumber(i);
            QTextCursor lc(b);
            lc.setPosition(b.position());
            if (dir > 0) lc.insertText(QStringLiteral("    "));
            else {
                QString text = b.text();
                int n = 0;
                while (n < 4 && n < text.size() && text[n] == QLatin1Char(' ')) n++;
                if (n > 0) {
                    lc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, n);
                    lc.removeSelectedText();
                }
            }
        }
        c.endEditBlock();
    }

    // ---- analysis
    void scheduleAnalysis(int level) {
        // never let a cheaper pass overwrite the result of a deeper one on unchanged text
        if (rev_ == analyzedRev_ && level <= analyzedLevel_) return;
        pendingLevel_ = std::max(pendingLevel_, level);
        if (!timer_) {
            timer_ = new QTimer(this);
            timer_->setSingleShot(true);
            connect(timer_, &QTimer::timeout, this, [this] {
                int level = pendingLevel_;
                pendingLevel_ = 1;
                analyzeNow(level);
            });
        }
        timer_->start(level >= 2 ? 0 : 260);
    }

    void analyzeNow(int level) {
        AnalysisOptions opts;
        opts.level = level;
        opts.collectIde = level >= 2;
        std::string path = path_.isEmpty() ? std::string("<editor>") : path_.toStdString();
        result_ = analyzeSource(editor_->toPlainText().toStdString(), path, opts);
        analyzedRev_ = rev_;
        analyzedLevel_ = level;
        diagnostics_ = result_.diagnostics;
        editor_->setDiagnostics(diagnostics_);
        updateProblems();
        updateStructure();
        updateCoverage();
        updateStatus();
    }

    void updateProblems() {
        problems_->setRowCount((int)diagnostics_.size());
        for (int i = 0; i < (int)diagnostics_.size(); i++) {
            const Diagnostic& d = diagnostics_[(size_t)i];
            QString sev = d.suppressed ? QStringLiteral("忽略")
                                       : (d.severity == Severity::Error ? QStringLiteral("✗ 错误")
                                                                        : (d.severity == Severity::Warning ? QStringLiteral("? 警告")
                                                                                                           : QStringLiteral("i 提示")));
            auto* sevItem = new QTableWidgetItem(sev);
            if (d.suppressed) sevItem->setForeground(QColor(0x99, 0x99, 0x99));
            else if (d.severity == Severity::Error) sevItem->setForeground(QColor(0xC0, 0x20, 0x20));
            else if (d.severity == Severity::Warning) sevItem->setForeground(QColor(0xB0, 0x70, 0x00));
            else sevItem->setForeground(QColor(0x30, 0x60, 0xA0));
            problems_->setItem(i, 0, sevItem);
            problems_->setItem(i, 1, new QTableWidgetItem(QString::number(d.line)));
            problems_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(d.code)));
            auto* msg = new QTableWidgetItem(QString::fromStdString(d.message));
            QString tip = QString::fromStdString(d.detail);
            if (!d.fix.empty()) tip += QStringLiteral("\n修复: ") + QString::fromStdString(d.fix);
            if (d.suppressed) tip += QStringLiteral("\n(被 [[ignore]] 抑制)");
            msg->setToolTip(tip);
            problems_->setItem(i, 3, msg);
        }
    }

    void updateStructure() {
        structure_->clear();
        for (auto& f : result_.functions) {
            QString mark = f.status == 'v' ? QStringLiteral("✓") : (f.status == 'x' ? QStringLiteral("✗") : QStringLiteral("?"));
            QString name = QString::fromStdString(f.name);
            auto* item = new QTreeWidgetItem(QStringList{mark + QStringLiteral("  ") + name, QString()});
            item->setData(0, Qt::UserRole, f.line);
            if (f.status == 'x') item->setForeground(0, QColor(0xC0, 0x20, 0x20));
            else if (f.status == 'v') item->setForeground(0, QColor(0x20, 0x80, 0x40));
            else item->setForeground(0, QColor(0x99, 0x77, 0x00));
            QString contract;
            if (f.hasRequire) contract += QStringLiteral("require ");
            if (f.hasEnsure) contract += QStringLiteral("ensure ");
            if (f.hasModifies) contract += QStringLiteral("modifies ");
            if (f.pure) contract += QStringLiteral("pure ");
            if (f.trusted) contract += QStringLiteral("trusted ");
            if (contract.isEmpty()) contract = QStringLiteral("(无)");
            contract += QStringLiteral("  L%1").arg(f.line);
            if (f.errors || f.warnings)
                contract += QStringLiteral("  %1✗ %2?").arg(f.errors).arg(f.warnings);
            item->setText(1, contract);
            structure_->addTopLevelItem(item);
        }
        structure_->resizeColumnToContents(0);
    }

    void updateCoverage() {
        const Coverage& c = result_.coverage;
        QString html = QStringLiteral("<h3>覆盖率</h3><table cellpadding='4'>");
        auto row = [&](const QString& name, int have, int total) {
            int pct = total ? (100 * have / total) : 100;
            QString color = pct >= 80 ? QStringLiteral("#2E7D32") : (pct >= 40 ? QStringLiteral("#B26A00") : QStringLiteral("#C62828"));
            html += QStringLiteral("<tr><td>%1</td><td><b style='color:%2'>%3 / %4 (%5%)</b></td></tr>")
                        .arg(name).arg(color).arg(have).arg(total).arg(pct);
        };
        row(QStringLiteral("函数契约"), c.withAny, c.functions);
        row(QStringLiteral("require"), c.withRequire, c.functions);
        row(QStringLiteral("ensure"), c.withEnsure, c.functions);
        row(QStringLiteral("modifies"), c.withModifies, c.functions);
        row(QStringLiteral("循环不变量"), c.loopsWithInvariant, c.loops);
        html += QStringLiteral("</table>");
        if (!c.uncovered.empty()) {
            html += QStringLiteral("<h4>还没有契约的函数</h4><ul>");
            for (size_t i = 0; i < c.uncovered.size() && i < 60; i++)
                html += QStringLiteral("<li>%1</li>").arg(QString::fromStdString(c.uncovered[i]));
            html += QStringLiteral("</ul>");
        }
        if (!result_.suppressions.empty()) {
            html += QStringLiteral("<h4>抑制（[[ignore]]）</h4><ul>");
            for (auto& s : result_.suppressions) {
                QString codes;
                for (auto& c2 : s.codes) codes += (codes.isEmpty() ? "" : ", ") + QString::fromStdString(c2);
                if (codes.isEmpty()) codes = QStringLiteral("全部");
                html += QStringLiteral("<li>第 %1 行：%2 %3</li>")
                            .arg(s.line).arg(codes)
                            .arg(s.used ? QStringLiteral("(生效)") : QStringLiteral("<span style='color:#C62828'>(未使用)</span>"));
            }
            html += QStringLiteral("</ul>");
        }
        if (!result_.fixes.empty()) {
            html += QStringLiteral("<h4>可用的快速修复</h4><ul>");
            for (auto& f : result_.fixes)
                html += QStringLiteral("<li>第 %1 行 &lt;%2&gt;：%3</li>")
                            .arg(f.line).arg(QString::fromStdString(f.code)).arg(QString::fromStdString(f.title));
            html += QStringLiteral("</ul>");
        }
        coverage_->setHtml(html);
    }

    void updateStatus() {
        statusLeft_->setText(QStringLiteral("%1 行 · %2 个诊断（%3 错误 / %4 警告） · %5 ms")
                                 .arg(result_.lines)
                                 .arg(result_.diagnostics.size())
                                 .arg(result_.errors)
                                 .arg(result_.warnings)
                                 .arg(result_.elapsedMs));
        int verified = 0, unknown = 0, broken = 0;
        for (auto& f : result_.functions) {
            if (f.status == 'v') verified++;
            else if (f.status == 'x') broken++;
            else unknown++;
        }
        statusRight_->setText(QStringLiteral("函数 ✓%1 ?%2 ✗%3").arg(verified).arg(unknown).arg(broken));
        statusBar()->setStyleSheet(result_.errors ? QStringLiteral("QStatusBar{background:#FDECEA;}")
                                                  : QStringLiteral(""));
    }

    void updateHover() {
        if (!hover_) return;
        QTextCursor cur = editor_->textCursor();
        int line = cur.blockNumber() + 1;
        int col = cur.positionInBlock() + 1;
        std::string path = path_.isEmpty() ? std::string("<editor>") : path_.toStdString();
        AnalysisOptions opts;
        opts.level = 2;
        HoverInfo h = hoverAt(editor_->toPlainText().toStdString(), path, line, col, opts);
        QString text;
        if (h.valid) {
            text = QStringLiteral("<b>%1</b> &nbsp; %2").arg(QString::fromStdString(h.title), QString::fromStdString(h.name));
            QString body = QString::fromStdString(h.body);
            body.replace(QLatin1Char('\n'), QLatin1String("<br>"));
            text += QStringLiteral("<br><span style='color:#555'>%1</span>").arg(body);
        } else {
            text = QStringLiteral("<span style='color:#888'>第 %1 行第 %2 列：把光标移到标注、变量或函数上查看文档</span>")
                       .arg(line).arg(col);
        }
        // also surface the diagnostics of this line
        QString here;
        for (auto& d : diagnostics_) {
            if (d.line == line && !d.suppressed)
                here += QStringLiteral("<br><span style='color:#B00020'>✗ %1: %2</span>")
                            .arg(QString::fromStdString(d.code), QString::fromStdString(d.message));
        }
        hover_->setText(text + here);
    }

    // ---- run / fixes
    void runCurrent(bool contracts = false) {
        std::string path = path_.isEmpty() ? std::string("<editor>") : path_.toStdString();
        RunOutcome out = runBuffer(editor_->toPlainText().toStdString(), path, contracts);
        QString text = QStringLiteral("== 运行 %1 %2 ==\n")
                           .arg(QString::fromStdString(path),
                                contracts ? QStringLiteral("(--contracts)") : QString())
                           .prepend(QStringLiteral("[%1] ").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
        if (!out.output.empty()) text += QString::fromStdString(out.output);
        if (!out.ok) {
            text += QStringLiteral("\n[错误] ");
            if (out.errorLine > 0) text += QStringLiteral("第 %1 行: ").arg(out.errorLine);
            text += QString::fromStdString(out.error) + QLatin1Char('\n');
        }
        if (out.ok && !out.views.empty())
            text += QStringLiteral("[提示] 这个程序定义了界面 %1，按 F7（运行菜单 → 预览界面）打开窗口\n")
                        .arg(QString::fromStdString(out.views.back()));
        text += QStringLiteral("-- 用时 %1 ms --\n\n").arg(out.elapsedMs);
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(text);
        output_->moveCursor(QTextCursor::End);
        statusBar()->showMessage(out.ok ? QStringLiteral("运行完成 (%1 ms)").arg(out.elapsedMs)
                                        : QStringLiteral("运行失败：%1").arg(QString::fromStdString(out.error)),
                                5000);
        if (!out.ok && out.errorLine > 0) editor_->goToLine(out.errorLine);
        // remember the program's view so that F7 can open it without running twice
        lastRun_ = out.vm;
        lastViews_ = out.views;
    }

    // Opens the program's own `view` in a window, using the IDE's QApplication and event loop.
    void previewView() {
        if (!lastRun_ || lastViews_.empty()) {
            RunOutcome out = runBuffer(editor_->toPlainText().toStdString(),
                                       path_.isEmpty() ? std::string("<editor>") : path_.toStdString(), false);
            lastRun_ = out.vm;
            lastViews_ = out.views;
            if (!out.ok) {
                statusBar()->showMessage(QStringLiteral("程序运行失败，无法预览：%1")
                                             .arg(QString::fromStdString(out.error)), 8000);
                return;
            }
        }
        if (lastViews_.empty()) {
            QMessageBox::information(
                this, QStringLiteral("没有界面可以预览"),
                QStringLiteral("这个程序里没有 `view`，所以没有窗口可以打开。\n\n"
                               "在 Annota 里，图形界面要用 view 声明，例如：\n\n"
                               "    view Counter(title=\"Counter\", width=400, height=300)(\n"
                               "        Column(pad=16)(\n"
                               "            Text(text=\"Count: \" + str(count))\n"
                               "            Button(label=\"+1\", click=inc)\n"
                               "        )\n"
                               "    )\n\n"
                               "参考 examples/gui_counter.ant。"));
            return;
        }
        std::string name = lastViews_.back();
        int rc = guiShowView(*lastRun_, name);
        if (rc == 0)
            statusBar()->showMessage(QStringLiteral("已打开界面 %1 的窗口（state 变化会自动重绘）")
                                         .arg(QString::fromStdString(name)), 6000);
        else
            statusBar()->showMessage(QStringLiteral("无法打开界面 %1（错误码 %2）")
                                         .arg(QString::fromStdString(name)).arg(rc), 8000);
    }

    // text rendering of the component tree, handy when a window cannot be shown
    void dumpViewTree() {
        if (!lastRun_ || lastViews_.empty()) {
            RunOutcome out = runBuffer(editor_->toPlainText().toStdString(),
                                       path_.isEmpty() ? std::string("<editor>") : path_.toStdString(), false);
            lastRun_ = out.vm;
            lastViews_ = out.views;
        }
        if (lastViews_.empty()) {
            statusBar()->showMessage(QStringLiteral("这个程序没有 view"), 6000);
            return;
        }
        output_->moveCursor(QTextCursor::End);
        for (auto& name : lastViews_) {
            Value cls;
            if (!lastRun_->getGlobal(name, cls) || cls.t != VT::Class) continue;
            try {
                Value tree = lastRun_->callSync(cls, {});
                output_->insertPlainText(QStringLiteral("\n== 组件树 %1 ==\n%2")
                                             .arg(QString::fromStdString(name),
                                                  QString::fromStdString(renderTree(tree))));
            } catch (VMError& e) {
                output_->insertPlainText(QStringLiteral("\n== %1 构建失败: %2 ==\n")
                                             .arg(QString::fromStdString(name), QString::fromStdString(e.message)));
            }
        }
        output_->moveCursor(QTextCursor::End);
    }

    void applyFix(int row) {
        if (row < 0 || row >= (int)diagnostics_.size()) {
            statusBar()->showMessage(QStringLiteral("先在“问题”里选中一行"), 4000);
            return;
        }
        const Diagnostic& d = diagnostics_[(size_t)row];
        if (d.fix.empty()) {
            statusBar()->showMessage(QStringLiteral("这条诊断没有可自动插入的修复"), 4000);
            return;
        }
        QString ins = QString::fromStdString(d.fix);
        QRegularExpression re(QStringLiteral("\\[\\[[^\\]]+\\]\\]"));
        auto m = re.match(ins);
        if (!m.hasMatch()) {
            statusBar()->showMessage(QStringLiteral("修复建议：%1").arg(ins), 8000);
            return;
        }
        QTextBlock b = editor_->document()->findBlockByNumber(std::max(0, d.line - 1));
        int indent = 0;
        QString lineText = b.text();
        while (indent < lineText.size() && lineText[indent] == QLatin1Char(' ')) indent++;
        QTextCursor cur(b);
        cur.setPosition(b.position());
        cur.insertText(QString(indent, QLatin1Char(' ')) + m.captured(0) + QLatin1Char('\n'));
        statusBar()->showMessage(QStringLiteral("已插入 %1").arg(m.captured(0)), 4000);
        scheduleAnalysis(3);
    }

    void applyAllFixes() {
        if (result_.fixes.empty()) {
            statusBar()->showMessage(QStringLiteral("没有可自动插入的修复"), 4000);
            return;
        }
        int applied = 0;
        for (auto& f : result_.fixes) {
            QString ins = QString::fromStdString(f.insertText);
            if (ins.isEmpty()) continue;
            QTextBlock b = editor_->document()->findBlockByNumber(std::max(0, f.line - 1));
            int indent = 0;
            QString lineText = b.text();
            while (indent < lineText.size() && lineText[indent] == QLatin1Char(' ')) indent++;
            QTextCursor cur(b);
            cur.setPosition(b.position());
            cur.insertText(QString(indent, QLatin1Char(' ')) + ins + QLatin1Char('\n'));
            applied++;
        }
        statusBar()->showMessage(QStringLiteral("插入了 %1 条修复").arg(applied), 5000);
        scheduleAnalysis(3);
    }

    void showFileApi() {
        showHtmlDialog(this, QStringLiteral("文件 API"),
                       QStringLiteral(
                           "<h2>文件 API</h2>"
                           "<p>底层：<code>_file_*</code> / <code>_dir_*</code> / <code>_path_*</code> 原语，"
                           "直接调用操作系统，出错即抛出。</p>"
                           "<p>上层：<code>use file</code> 之后的 <code>File</code> / <code>Dir</code> / "
                           "<code>Path</code> / <code>Text</code> / <code>Result</code>。</p>"
                           "<h3>File</h3><pre>"
                           "File.read(p)            File.read_or(p, \"兜底\")     File.try_read(p) -> Result\n"
                           "File.write(p, text)     File.append(p, text)       File.append_line(p, line)\n"
                           "File.lines(p)           File.line(p, n)            File.write_lines(p, xs)\n"
                           "File.json(p)            File.write_json(p, v)      File.csv(p)\n"
                           "File.exists(p)          File.is_file(p)            File.is_dir(p)\n"
                           "File.size(p)            File.mtime(p)              File.describe(p)\n"
                           "File.copy(a, b)         File.move(a, b)            File.remove(p)\n"
                           "File.touch(p)           File.unique(prefix)\n"
                           "</pre>"
                           "<h3>Dir</h3><pre>"
                           "Dir.list(p)   Dir.files(p)   Dir.dirs(p)   Dir.count(p)\n"
                           "Dir.make(p)   Dir.remove(p, recursive)   Dir.clear(p)\n"
                           "Dir.walk(p)   Dir.find(p, \".ant\")   Dir.find_in(p, \".ant\")   Dir.size_of(p)\n"
                           "</pre>"
                           "<h3>Path</h3><pre>"
                           "Path.join(a, b[, c])   Path.dirname(p)   Path.basename(p)   Path.ext(p)\n"
                           "Path.stem(p)   Path.with_ext(p, ext)   Path.abs(p)   Path.normalize(p)\n"
                           "Path.is_abs(p)   Path.sibling(p, name)   Path.parts(p)   Path.join_all(xs)\n"
                           "</pre>"
                           "<h3>Text</h3><pre>"
                           "Text.to_lines(s)   Text.from_lines(xs)   Text.count_lines(s)\n"
                           "</pre>"
                           "<p>例子见 <code>examples/files.ant</code>。</p>"));
    }

    void showShortcuts() {
        showHtmlDialog(this, QStringLiteral("快捷键"),
                       QStringLiteral("<h2>快捷键</h2><table cellpadding='4'>"
                                      "<tr><td>F5</td><td>运行当前缓冲区</td></tr>"
                                      "<tr><td>Ctrl+F5</td><td>带契约检查运行</td></tr>"
                                      "<tr><td>F6</td><td>完整静态分析（3 层）</td></tr>"
                                      "<tr><td>F1 / F2</td><td>标注手册 / 检查手册</td></tr>"
                                      "<tr><td>Ctrl+N / O / S</td><td>新建 / 打开 / 保存</td></tr>"
                                      "<tr><td>Ctrl+/</td><td>注释或取消注释</td></tr>"
                                      "<tr><td>Ctrl+] / Ctrl+[</td><td>增加 / 减少缩进</td></tr>"
                                      "<tr><td>Ctrl+1</td><td>把选中问题的修复插入到所在行</td></tr>"
                                      "<tr><td>Ctrl+Shift+1</td><td>插入全部可自动插入的修复</td></tr>"
                                      "<tr><td>Tab</td><td>插入四个空格；输入 ( 或 [ 自动配对</td></tr>"
                                      "<tr><td>双击问题 / 结构</td><td>跳转到对应行</td></tr>"
                                      "</table>"));
    }

    void showAbout() {
        QString qt = QString::fromLatin1(qVersion());
        showHtmlDialog(this, QStringLiteral("关于 Annota Studio"),
                       QStringLiteral("<h2>Annota Studio</h2>"
                                      "<p>Annota 语言的图形化 IDE：编辑器 + 静态分析（第 %1 章的三层检查）+ 运行器。</p>"
                                      "<p>Qt 版本 %2；本窗口由 <code>annota studio</code> 启动。</p>"
                                      "<p>命令行等价物：<code>annota analyze</code>、<code>annota ide</code>、"
                                      "<code>annota repl</code>、<code>annota lsp</code>。</p>"
                                      "<p>用 <code>annota --features</code> 查看本构建的能力。</p>")
                           .arg(6).arg(qt));
    }

    Editor* editor_ = nullptr;
    Highlighter* highlighter_ = nullptr;
    QTableWidget* problems_ = nullptr;
    QTreeWidget* structure_ = nullptr;
    QTextBrowser* coverage_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QLabel* hover_ = nullptr;
    QLabel* statusLeft_ = nullptr;
    QLabel* statusRight_ = nullptr;
    QSplitter* split_ = nullptr;
    QTabWidget* rightTabs_ = nullptr;
    QTimer* timer_ = nullptr;
    AnalysisResult result_;
    std::vector<Diagnostic> diagnostics_;
    std::shared_ptr<VM> lastRun_;          // VM of the last run, kept alive for view previews
    std::vector<std::string> lastViews_;   // view classes the last run defined
    QString path_;
    bool dirty_ = false;
    bool loading_ = false;
    int pendingLevel_ = 1;
    int rev_ = 0;               // bumped on every text change
    int analyzedRev_ = -1;      // revision the current result was computed from
    int analyzedLevel_ = 0;     // level of the current result
};

int runStudio(const std::string& file, const std::string& shot, const std::string& tab, bool autorun,
              bool autopreview, bool echo) {
    static int argc = 1;
    static char name[] = "annota-studio";
    static char* argv[] = {name, nullptr};
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Annota Studio"));
    Studio win(QString::fromStdString(file), QString::fromStdString(tab));
    win.show();
    if (autorun) QTimer::singleShot(80, &win, [&win, echo] { win.runForTest(echo); });
    if (autopreview) QTimer::singleShot(120, &win, [&win, echo] { win.previewForTest(echo); });
    if (!shot.empty()) {
        // headless verification: give the window a moment to lay out, save a PNG, then quit
        QString out = QString::fromStdString(shot);
        QTimer::singleShot(700, &win, [&win, out] {
            bool ok = win.grab().save(out);
            QStringList titles;
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->isVisible() && !w->windowTitle().isEmpty()) titles << w->windowTitle();
            std::fprintf(ok ? stdout : stderr, "annota studio: %s %s (windows: %s)\n",
                         ok ? "wrote" : "cannot write", out.toStdString().c_str(),
                         titles.join(QStringLiteral(", ")).toStdString().c_str());
            QApplication::quit();
        });
    }
    return app.exec();
}

} // namespace

int cmdStudio(int argc, char** argv) {
    std::string file;
    std::string shot;
    std::string tab;
    bool autorun = false;
    bool autopreview = false;
    bool echo = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) { shot = argv[++i]; continue; }
        if (a.rfind("--tab=", 0) == 0) { tab = a.substr(6); continue; }
        if (a == "--run") { autorun = true; continue; }
        if (a == "--preview") { autopreview = true; continue; }
        if (a == "--echo") { echo = true; continue; }
        if (!a.empty() && a[0] != '-') file = a;
    }
    return runStudio(file, shot, tab, autorun, autopreview, echo);
}

} // namespace annota

#else  // ------------------------------------------------------------- no Qt in this build

namespace annota {
int cmdStudio(int, char**) {
    std::fprintf(stderr,
                 "annota studio: 这个二进制没有 Qt 支持，无法打开图形界面。\n"
                 "  重新构建并指定 Qt：\n"
                 "    powershell -ExecutionPolicy Bypass -File build.ps1 -QtRoot D:\\Qt\\6.10.1\\mingw_64\n"
                 "  没有 Qt 时可用：annota repl / annota analyze / annota ide <query>\n");
    return 2;
}
} // namespace annota

#endif
