// Annota - gui_qt.cpp : declarative views rendered into a real Qt window.
//
// The component tree produced by a `view` / component class is laid out and painted by a single
// custom widget: the tree is rebuilt from the view class whenever a state variable changes, which
// is exactly the reactive model described in the language documentation.
//
// Only compiled when ANNOTA_QT is defined (see build.ps1 / CMakeLists.txt).
#include "gui.hpp"

#ifdef ANNOTA_QT

#include "builtins.hpp"
#include <cstdlib>
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QFontMetrics>
#include <QInputDialog>
#include <QLineEdit>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QPixmap>
#include <QHash>
#include <QVector>
#include <QImage>
#include <cstdio>
#include <vector>
#include <algorithm>

namespace annota {
namespace {

QString S(const std::string& s) { return QString::fromUtf8(s.c_str()); }

// ---------------------------------------------------------------- attribute access
const Value* attrOf(const Value& n, const char* k) {
    if (n.t != VT::UiNode || !n.o) return nullptr;
    auto it = n.o->map.find(k);
    return it == n.o->map.end() ? nullptr : &it->second;
}

int iattr(const Value& n, const char* k, int d) {
    const Value* v = attrOf(n, k);
    if (!v) return d;
    if (v->t == VT::Int) return (int)v->i;
    if (v->t == VT::Float) return (int)v->f;
    return d;
}

bool battr(const Value& n, const char* k, bool d = false) {
    const Value* v = attrOf(n, k);
    if (!v) return d;
    if (v->t == VT::Bool) return v->b;
    if (v->t == VT::Int) return v->i != 0;
    if (v->t == VT::Str) return v->o->str == "true" || v->o->str == "wrap" || v->o->str == "yes";
    return d;
}

QString sattr(const Value& n, const char* k, const QString& d = QString()) {
    const Value* v = attrOf(n, k);
    if (!v) return d;
    if (v->t == VT::Str) return S(v->o->str);
    return d;
}

QColor cattr(const Value& n, const char* k, const QColor& d) {
    const Value* v = attrOf(n, k);
    if (!v) return d;
    if (v->t == VT::Color) return QColor::fromRgb((v->o->color >> 16) & 0xFF, (v->o->color >> 8) & 0xFF, v->o->color & 0xFF);
    if (v->t == VT::Str) return QColor(S(v->o->str));
    return d;
}

struct Style {
    int pad = 0, gap = 0, w = -1, h = -1, size = 14, radius = 6, border = 0;
    bool bold = false, light = false, disabled = false, visible = true, wrap = false;
    QColor fg{26, 26, 46}, bg;
    bool hasBg = false;
    QString align, text, label, src, direction, value, placeholder, href;
    bool isCenter = false, isEnd = false;
};

Style styleOf(const Value& n) {
    Style st;
    st.pad = iattr(n, "pad", 0);
    st.gap = iattr(n, "gap", 0);
    st.w = iattr(n, "width", -1);
    st.h = iattr(n, "height", -1);
    st.size = iattr(n, "size", 14);
    st.radius = iattr(n, "radius", 6);
    st.border = iattr(n, "border", 0);
    st.disabled = battr(n, "disabled");
    st.visible = battr(n, "visible", true);
    st.wrap = battr(n, "wrap");
    st.fg = cattr(n, "color", QColor(26, 26, 46));
    const Value* bgv = attrOf(n, "bg");
    if (!bgv) bgv = attrOf(n, "background");
    if (bgv) { st.hasBg = true; st.bg = cattr(n, "bg", cattr(n, "background", Qt::transparent)); }
    QString weight = sattr(n, "weight");
    st.bold = (weight == "bold");
    st.light = (weight == "light");
    st.align = sattr(n, "align");
    st.isCenter = (st.align == "center");
    st.isEnd = (st.align == "end" || st.align == "right" || st.align == "bottom");
    st.direction = sattr(n, "direction");
    st.src = sattr(n, "src");
    st.text = sattr(n, "text");
    st.label = sattr(n, "label");
    st.value = sattr(n, "value");
    st.placeholder = sattr(n, "placeholder");
    if (!attrOf(n, "arg0")) { /* nothing */ }
    const Value* a0 = attrOf(n, "arg0");
    if (a0 && a0->t == VT::Str) {
        QString s = S(a0->o->str);
        if (st.text.isEmpty()) st.text = s;
        if (st.label.isEmpty()) st.label = s;
        if (st.placeholder.isEmpty()) st.placeholder = s;
    }
    return st;
}

QFont fontOf(const Style& st) {
    QFont f(QStringLiteral("Segoe UI"));
    f.setPixelSize(st.size > 0 ? st.size : 14);
    if (st.bold) f.setBold(true);
    else if (st.light) f.setWeight(QFont::Light);
    return f;
}

int alignOffset(const QString& align, int avail, int size, bool cross) {
    if (align == "center") return (avail - size) / 2;
    if (align == "end" || align == "right" || align == "bottom") return avail - size;
    (void)cross;
    return 0;
}

// ---------------------------------------------------------------- the canvas
class ViewWindow : public QWidget {
public:
    ViewWindow(VM& vm, Value cls) : vm_(vm), cls_(std::move(cls)) {
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, QColor(255, 255, 255));
        setPalette(pal);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        vm_.onStateChange = [this](VM&) { dirty_ = true; };
        rebuild();
    }

    void rebuild() {
        dirty_ = false;
        meas_.clear();
        items_.clear();
        scroll_.clear();
        try {
            Value tree = vm_.callSync(cls_, {});
            if (tree.t == VT::List || tree.t == VT::Tuple) {
                if (tree.o->items.size() == 1) root_ = tree.o->items[0];
                else {
                    Value col = makeUiNode("Column", {});
                    for (auto& c : tree.o->items) col.o->items.push_back(c);
                    root_ = col;
                }
            } else {
                root_ = tree;
            }
        } catch (VMError& e) {
            std::fprintf(stderr, "annota: view error: %s\n", e.message.c_str());
            root_ = Value::null();
        }
        if (root_.t != VT::UiNode) {
            resize(320, 120);
            return;
        }
        QSize sz = measure(root_, 1 << 20);
        if (!sized_) {
            sized_ = true;
            resize(sz.expandedTo(QSize(160, 80)));
        }
        setWindowTitle(sattr(root_, "title", QStringLiteral("Annota")));
        restoreFocus();
        relayout();
    }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        relayout();
    }

    void paintEvent(QPaintEvent*) override {
        if (dirty_) rebuild();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(255, 255, 255));
        for (auto& it : items_) {
            if (!it.clip.isNull()) p.setClipRect(it.clip);
            else p.setClipping(false);
            paintNode(p, it);
        }
        p.setClipping(false);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        int i = hitTest(e->pos(), true);
        pressed_ = (i >= 0) ? items_[(size_t)i].node.o.get() : nullptr;
        if (i >= 0 && S(items_[(size_t)i].node.o->str) == "Input") {
            const Value& node = items_[(size_t)i].node;
            editText_ = sattr(node, "value");
            focused_ = node.o.get();
            focusedKey_ = inputKey(node);
        }
        update();
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        int i = hitTest(e->pos(), true);
        const Obj* o = (i >= 0) ? items_[(size_t)i].node.o.get() : nullptr;
        if (o && o == pressed_ && S(o->str) == "Button") {
            callHandler(items_[(size_t)i].node, "click", {});
        }
        pressed_ = nullptr;
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        int i = hitTest(e->pos(), true);
        const Obj* o = (i >= 0) ? items_[(size_t)i].node.o.get() : nullptr;
        if (o != hovered_) {
            hovered_ = o;
            setCursor(o ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void leaveEvent(QEvent*) override {
        if (hovered_) { hovered_ = nullptr; update(); }
    }

    void keyPressEvent(QKeyEvent* e) override {
        if (!focused_) { QWidget::keyPressEvent(e); return; }
        if (e->key() == Qt::Key_Backspace) {
            if (!editText_.isEmpty()) editText_.chop(1);
            notifyChange();
            update();
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            Value node = nodeFor(focused_);
            if (node.t == VT::UiNode) callHandler(node, "submit", {Value::str(editText_.toUtf8().constData())});
            return;
        }
        if (e->key() == Qt::Key_Escape) {
            focused_ = nullptr;
            update();
            return;
        }
        QString t = e->text();
        if (!t.isEmpty() && t.at(0).isPrint()) {
            editText_ += t;
            notifyChange();
            update();
        }
    }

    void wheelEvent(QWheelEvent* e) override {
        QPoint pos = e->position().toPoint();
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (!it->clip.contains(pos) || !it->rect.contains(pos)) continue;
            if (S(it->node.o->str) != "Scroll") continue;
            int delta = e->angleDelta().y() / 3;
            int off = scroll_.value(it->node.o.get(), 0) - delta;
            scroll_[it->node.o.get()] = std::max(0, off);
            relayout();
            update();
            return;
        }
    }

private:
    struct Item {
        Value node;
        QRect rect;
        QRect clip;
    };

    VM& vm_;
    Value cls_;
    Value root_;
    bool dirty_ = true;
    bool sized_ = false;
    std::vector<Item> items_;
    QHash<const Obj*, QSize> meas_;
    QHash<const Obj*, int> scroll_;
    QHash<QString, QPixmap> images_;
    const Obj* hovered_ = nullptr;
    const Obj* pressed_ = nullptr;
    const Obj* focused_ = nullptr;
    QString focusedKey_;
    QString editText_;

    // ---- view model
    QString inputKey(const Value& n) {
        QString k = sattr(n, "name");
        if (!k.isEmpty()) return k;
        k = sattr(n, "text");
        if (!k.isEmpty()) return k;
        k = sattr(n, "placeholder");
        return k;
    }
    QString inputValue(const Value& n) {
        if (focused_ && n.o.get() == focused_) return editText_;
        return sattr(n, "value");
    }
    void restoreFocus() {
        focused_ = nullptr;
        if (focusedKey_.isEmpty()) return;
        collect(root_);
        for (auto& it : items_) {
            if (S(it.node.o->str) == "Input" && inputKey(it.node) == focusedKey_) {
                focused_ = it.node.o.get();
                break;
            }
        }
    }
    Value nodeFor(const Obj* o) {
        for (auto& it : items_) if (it.node.o.get() == o) return it.node;
        return Value::null();
    }
    void notifyChange() {
        Value node = nodeFor(focused_);
        if (node.t != VT::UiNode) return;
        callHandler(node, "change", {Value::str(editText_.toUtf8().constData())});
    }
    void callHandler(const Value& node, const char* key, std::vector<Value> args) {
        const Value* h = attrOf(node, key);
        if (!h || (h->t != VT::Function && h->t != VT::Native && h->t != VT::Bound)) return;
        try {
            vm_.callSync(*h, std::move(args));
        } catch (VMError& e) {
            std::fprintf(stderr, "annota: %s handler error: %s\n", key, e.message.c_str());
        }
        dirty_ = true;
    }

    // ---- measuring
    QSize measure(const Value& n, int availW) {
        if (n.t != VT::UiNode) return QSize(0, 0);
        if (availW >= (1 << 20)) {
            auto c = meas_.constFind(n.o.get());
            if (c != meas_.constEnd()) return c.value();
        }
        Style st = styleOf(n);
        QString type = S(n.o->str);
        QSize sz(0, 0);
        if (type == "Text") {
            QFontMetrics fm(fontOf(st));
            QString t = st.text;
            int maxW = st.w > 0 ? st.w : (availW < (1 << 20) ? std::max(40, availW - 2 * st.pad) : (1 << 20));
            int flags = st.wrap ? Qt::TextWordWrap : 0;
            QRect br = fm.boundingRect(QRect(0, 0, maxW, 1 << 20), flags, t);
            sz = QSize(br.width(), br.height());
        } else if (type == "Button") {
            QFontMetrics fm(fontOf(st));
            sz = QSize(fm.horizontalAdvance(st.label) + 26, fm.height() + 12);
        } else if (type == "Input") {
            QFontMetrics fm(fontOf(st));
            sz = QSize(st.w > 0 ? st.w : 180, fm.height() + 12);
        } else if (type == "Image") {
            int s = st.size > 0 ? st.size : 32;
            QPixmap pm = image(st.src);
            QSize natural = pm.isNull() ? QSize(s, s) : pm.size().scaled(s, s, Qt::KeepAspectRatio);
            sz = natural;
        } else if (type == "Spacer") {
            sz = QSize(0, 0);
        } else if (type == "Scroll") {
            QSize c = n.o->items.empty() ? QSize(0, 0) : measure(n.o->items[0], availW);
            sz = c + QSize(2 * st.pad + 2, 2 * st.pad + 2);
        } else if (type == "Stack") {
            for (auto& c : n.o->items) sz = sz.expandedTo(measure(c, availW - 2 * st.pad));
            sz += QSize(2 * st.pad, 2 * st.pad);
        } else if (type == "Slider") {
            sz = QSize(st.w > 0 ? st.w : 140, 24);
        } else if (type == "Checkbox") {
            QFontMetrics fm(fontOf(st));
            sz = QSize(fm.horizontalAdvance(st.label) + 26, std::max(20, fm.height() + 4));
        } else {   // Column / Row
            bool col = (type != "Row");
            int main = 0, cross = 0;
            for (auto& c : n.o->items) {
                QSize cs = measure(c, col ? (availW < (1 << 20) ? availW - 2 * st.pad : (1 << 20)) : (1 << 20));
                if (col) { main += cs.height(); cross = std::max(cross, cs.width()); }
                else { main += cs.width(); cross = std::max(cross, cs.height()); }
            }
            if (!n.o->items.empty()) main += st.gap * ((int)n.o->items.size() - 1);
            sz = col ? QSize(cross, main) : QSize(main, cross);
            sz += QSize(2 * st.pad, 2 * st.pad);
        }
        if (st.w > 0) sz.setWidth(st.w);
        if (st.h > 0) sz.setHeight(st.h);
        if (availW >= (1 << 20)) meas_[n.o.get()] = sz;
        return sz;
    }

    void relayout() {
        items_.clear();
        if (root_.t != VT::UiNode) return;
        QRect box(0, 0, width(), height());
        layoutNode(root_, box, box);
    }

    void layoutNode(const Value& n, QRect r, QRect clip) {
        if (n.t != VT::UiNode) return;
        Style st = styleOf(n);
        if (!st.visible) return;
        items_.push_back({n, r, clip});
        QString type = S(n.o->str);
        QRect inner = r.adjusted(st.pad, st.pad, -st.pad, -st.pad);
        if (type == "Column" || type == "Row") {
            bool col = (type != "Row");
            std::vector<QSize> sizes;
            int total = 0, spacers = 0;
            for (auto& c : n.o->items) {
                QSize cs = measure(c, col ? inner.width() : (1 << 20));
                if (S(c.o->str) == "Spacer") spacers++;
                sizes.push_back(cs);
                total += col ? cs.height() : cs.width();
            }
            if (!n.o->items.empty()) total += st.gap * ((int)n.o->items.size() - 1);
            int leftover = (col ? inner.height() : inner.width()) - total;
            int extra = (spacers > 0 && leftover > 0) ? leftover / spacers : 0;
            int pos = col ? inner.top() : inner.left();
            for (size_t i = 0; i < n.o->items.size(); i++) {
                QSize cs = sizes[i];
                bool isSpacer = (S(n.o->items[i].o->str) == "Spacer");
                int main = col ? cs.height() : cs.width();
                if (isSpacer) main += extra;
                int cross = col ? cs.width() : cs.height();
                QRect cr;
                if (col) {
                    int x = inner.left() + alignOffset(st.align, inner.width(), cross, true);
                    cr = QRect(x, pos, cross, main);
                } else {
                    int y = inner.top() + alignOffset(st.align, inner.height(), cross, true);
                    cr = QRect(pos, y, main, cross);
                }
                layoutNode(n.o->items[i], cr, clip);
                pos += main + st.gap;
            }
        } else if (type == "Stack") {
            for (auto& c : n.o->items) layoutNode(c, inner, clip);
        } else if (type == "Scroll") {
            if (n.o->items.empty()) return;
            int off = scroll_.value(n.o.get(), 0);
            QRect child = inner;
            if (st.direction == "horizontal") child.moveLeft(inner.left() - off);
            else child.moveTop(inner.top() - off);
            layoutNode(n.o->items[0], child, clip.intersected(r));
        }
    }

    QPixmap image(const QString& src) {
        if (src.isEmpty()) return QPixmap();
        auto it = images_.find(src);
        if (it != images_.end()) return it.value();
        QPixmap pm(src);
        images_.insert(src, pm);
        return pm;
    }

    void collect(const Value& n) {
        // used before items_ exists (focus restoration needs an ordering pass)
        if (n.t == VT::UiNode) items_.push_back({n, QRect(), QRect()});
        if (n.t == VT::UiNode || n.t == VT::List || n.t == VT::Tuple)
            for (auto& c : n.o->items) collect(c);
    }

    // ---- painting
    void paintNode(QPainter& p, const Item& it) {
        const Value& n = it.node;
        Style st = styleOf(n);
        QString type = S(n.o->str);
        QRect r = it.rect;
        if (st.hasBg) {
            QPainterPath path;
            path.addRoundedRect(r, st.radius, st.radius);
            p.fillPath(path, st.bg);
        }
        if (type == "Text") {
            p.setFont(fontOf(st));
            p.setPen(st.fg);
            int flags = Qt::AlignVCenter | (st.wrap ? Qt::TextWordWrap : 0);
            flags |= st.isCenter ? Qt::AlignHCenter : (st.isEnd ? Qt::AlignRight : Qt::AlignLeft);
            p.drawText(r, flags, st.text);
        } else if (type == "Button") {
            bool hover = (hovered_ == n.o.get());
            bool down = (pressed_ == n.o.get());
            bool themed = (attrOf(n, "color") != nullptr) && !st.hasBg;
            QColor bg = st.hasBg ? st.bg : (themed ? QColor(45, 110, 220) : QColor(245, 246, 248));
            QColor fg = st.fg;
            if (themed && !attrOf(n, "color")) fg = QColor(255, 255, 255);
            if (st.disabled) bg = bg.lighter(103);
            else if (down) bg = bg.darker(108);
            else if (hover) bg = bg.lighter(104);
            QPainterPath path;
            path.addRoundedRect(r, st.radius, st.radius);
            p.fillPath(path, bg);
            p.setPen(QPen(themed ? bg.darker(115) : QColor(201, 204, 212), 1));
            p.drawRoundedRect(r.adjusted(0, 0, -1, -1), st.radius, st.radius);
            p.setFont(fontOf(st));
            p.setPen(st.disabled ? QColor(160, 163, 170) : fg);
            p.drawText(r, Qt::AlignCenter, st.label);
        } else if (type == "Input") {
            bool focus = (focused_ == n.o.get());
            QPainterPath path;
            path.addRoundedRect(r, st.radius, st.radius);
            p.fillPath(path, QColor(255, 255, 255));
            p.setPen(QPen(focus ? QColor(45, 110, 220) : QColor(201, 204, 212), focus ? 2 : 1));
            p.drawRoundedRect(r.adjusted(0, 0, -1, -1), st.radius, st.radius);
            p.setFont(fontOf(st));
            QString text = (focused_ == n.o.get()) ? editText_ : st.value;
            QRect inner = r.adjusted(8, 0, -8, 0);
            if (text.isEmpty() && !st.placeholder.isEmpty()) {
                p.setPen(QColor(150, 153, 160));
                p.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft, st.placeholder);
            } else {
                p.setPen(st.fg);
                p.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft, text);
            }
            if (focus) {
                int x = inner.left() + QFontMetrics(fontOf(st)).horizontalAdvance(text) + 1;
                p.setPen(QPen(QColor(45, 110, 220), 1));
                p.drawLine(x, r.top() + 4, x, r.bottom() - 4);
            }
        } else if (type == "Image") {
            QPixmap pm = image(st.src);
            if (!pm.isNull()) {
                QPixmap scaled = pm.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                p.drawPixmap(r.left() + (r.width() - scaled.width()) / 2,
                             r.top() + (r.height() - scaled.height()) / 2, scaled);
            } else {
                p.setPen(QPen(QColor(201, 204, 212), 1, Qt::DashLine));
                p.drawRect(r.adjusted(0, 0, -1, -1));
                if (r.width() >= 64 && r.height() >= 20) {
                    QFont f = fontOf(st);
                    f.setPixelSize(10);
                    p.setFont(f);
                    p.setPen(QColor(150, 153, 160));
                    p.drawText(r.adjusted(2, 0, -2, 0), Qt::AlignCenter,
                               st.src.isEmpty() ? QStringLiteral("image") : st.src);
                }
            }
        } else if (type == "Checkbox") {
            QRect box(r.left(), r.top() + (r.height() - 16) / 2, 16, 16);
            p.setPen(QPen(QColor(201, 204, 212), 1));
            p.setBrush(Qt::white);
            p.drawRoundedRect(box, 3, 3);
            if (battr(n, "checked")) {
                p.setPen(QPen(QColor(45, 110, 220), 2));
                p.drawLine(box.left() + 4, box.center().y(), box.left() + 7, box.bottom() - 5);
                p.drawLine(box.left() + 7, box.bottom() - 5, box.right() - 4, box.top() + 4);
            }
            p.setFont(fontOf(st));
            p.setPen(st.fg);
            p.drawText(QRect(box.right() + 8, r.top(), r.width() - 24, r.height()),
                       Qt::AlignVCenter | Qt::AlignLeft, st.label);
        } else if (type == "Slider") {
            int cy = r.center().y();
            p.setPen(QPen(QColor(210, 213, 220), 4));
            p.drawLine(r.left() + 6, cy, r.right() - 6, cy);
            double ratio = 0.5;
            const Value* v = attrOf(n, "value");
            if (v && v->t == VT::Float) ratio = std::max(0.0, std::min(1.0, v->f));
            else if (v && v->t == VT::Int) ratio = std::max(0.0, std::min(1.0, (double)v->i / 100.0));
            int x = r.left() + 6 + (int)((r.width() - 12) * ratio);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(45, 110, 220));
            p.drawEllipse(QPoint(x, cy), 7, 7);
        } else if (type == "Scroll") {
            p.setPen(QPen(QColor(228, 230, 235), 1));
            p.drawRect(r.adjusted(0, 0, -1, -1));
        }
    }

    // ---- hit testing
    int hitTest(QPoint pt, bool interactive) const {
        for (int i = (int)items_.size() - 1; i >= 0; i--) {
            const Item& it = items_[(size_t)i];
            if (!it.clip.isNull() && !it.clip.contains(pt)) continue;
            if (!it.rect.contains(pt)) continue;
            QString type = S(it.node.o->str);
            if (interactive) {
                bool ok = (type == "Button" || type == "Input" || type == "Checkbox" || type == "Slider" || type == "Link");
                if (!ok) continue;
            }
            return i;
        }
        return -1;
    }
};

// ---------------------------------------------------------------- Qt bootstrap
// Qt needs its platform plugin.  A deployed build finds it next to the executable
// (see build.ps1); setting QT_PLUGIN_PATH is the manual escape hatch.
void ensureQtPaths() {}

// QApplication wants a stable argv for its whole lifetime.
int& qtArgc() {
    static int argc = 0;
    return argc;
}
char** qtArgv() {
    static std::vector<char*> args;
    if (args.empty()) {
        args.push_back(const_cast<char*>("annota"));
        args.push_back(nullptr);
        qtArgc() = 1;
    }
    return args.data();
}

bool fetchView(VM& vm, const std::string& viewName, Value& cls) {
    if (!vm.getGlobal(viewName, cls) || cls.t != VT::Class) {
        std::fprintf(stderr, "annota: view '%s' is not defined\n", viewName.c_str());
        return false;
    }
    return true;
}

} // namespace

bool guiAvailable() { return true; }

int guiInit(int argc, char** argv) {
    ensureQtPaths();
    if (!qApp) {
        int n = qtArgc();
        new QApplication(n, qtArgv());
        (void)argc;
        (void)argv;
    }
    return 0;
}

void guiInstallInput(VM& vm) {
    vm.inputProvider = [](const std::string& hint) -> std::string {
        // headless runs (CI, screenshots) can pre-answer instead of showing a dialog
        const char* scripted = std::getenv("ANNOTA_INPUT");
        if (scripted) return std::string(scripted);
        if (!qApp) return std::string();          // no GUI available: give an empty line
        bool ok = false;
        QString title = QStringLiteral("输入");
        QString label = QString::fromStdString(hint.empty() ? std::string("input") : hint);
        QString text = QInputDialog::getText(nullptr, title, label + QStringLiteral(" ="), QLineEdit::Normal,
                                             QString(), &ok);
        return ok ? text.toStdString() : std::string();
    };
}

int guiRunView(VM& vm, const std::string& viewName) {
    ensureQtPaths();
    if (!qApp) guiInit(0, nullptr);
    Value cls;
    if (!fetchView(vm, viewName, cls)) return 2;
    ViewWindow win(vm, cls);
    win.show();
    win.raise();
    win.activateWindow();
    return qApp->exec();
}

int guiRun(VM& vm, const std::string& viewName, int argc, char** argv) {
    guiInit(argc, argv);
    guiInstallInput(vm);
    return guiRunView(vm, viewName);
}

int guiShowView(VM& vm, const std::string& viewName) {
    ensureQtPaths();
    if (!qApp) {
        std::fprintf(stderr, "annota: guiShowView needs a running QApplication (use --gui instead)\n");
        return 3;
    }
    Value cls;
    if (!fetchView(vm, viewName, cls)) return 2;
    ViewWindow* win = new ViewWindow(vm, cls);
    win->setAttribute(Qt::WA_DeleteOnClose, true);
    if (QWidget* owner = QApplication::activeWindow()) win->setWindowIcon(owner->windowIcon());
    win->show();
    win->raise();
    win->activateWindow();
    return 0;
}

int guiRenderPng(VM& vm, const std::string& viewName, const std::string& path,
                 const std::vector<std::pair<int, int>>& clicks,
                 const std::vector<std::string>& keys) {
    ensureQtPaths();
    int argc = qtArgc();
    QApplication app(argc, qtArgv());
    Value cls;
    if (!fetchView(vm, viewName, cls)) return 2;
    ViewWindow win(vm, cls);
    win.rebuild();
    for (auto& c : clicks) {
        QPointF pt(c.first, c.second);
        QMouseEvent press(QEvent::MouseButtonPress, pt, pt, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, pt, pt, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&win, &press);
        QApplication::sendEvent(&win, &release);
    }
    for (auto& text : keys) {
        for (char ch : text) {
            int key = (ch == '\n') ? Qt::Key_Return : 0;
            QString t = (ch == '\n') ? QString() : QString(QChar(QLatin1Char(ch)));
            QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier, t);
            QApplication::sendEvent(&win, &ev);
        }
    }
    QPixmap pm = win.grab();
    // on a HiDPI screen grab() returns physical pixels; store a logical sized image instead so
    // that the file matches the declared view width/height
    if (!pm.isNull() && (pm.width() != win.width() || pm.height() != win.height()))
        pm = pm.scaled(win.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (pm.isNull() || !pm.save(S(path), "PNG")) {
        std::fprintf(stderr, "annota: cannot write '%s'\n", path.c_str());
        return 1;
    }
    return 0;
}

void guiViewSize(VM& vm, const std::string& viewName, int& w, int& h, std::string& title) {
    w = h = 0;
    title.clear();
    Value cls;
    if (!vm.getGlobal(viewName, cls) || cls.t != VT::Class) return;
    auto k = cls.o->klass;
    for (const char* key : {"width", "height", "title"}) {
        int idx = k->findField(key);
        if (idx < 0 || idx >= (int)k->fieldDefaults.size()) continue;
        const Value& v = k->fieldDefaults[(size_t)idx];
        if (std::string(key) == "width" && v.t == VT::Int) w = (int)v.i;
        if (std::string(key) == "height" && v.t == VT::Int) h = (int)v.i;
        if (std::string(key) == "title" && v.t == VT::Str) title = v.o->str;
    }
}

} // namespace annota

#endif // ANNOTA_QT
