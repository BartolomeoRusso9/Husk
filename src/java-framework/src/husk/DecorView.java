package husk;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.*;
import android.graphics.drawable.Drawable;
import android.util.TypedValue;
import android.view.*;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * A window's decor: the window background, the status and navigation bar areas (painted in their colours unless the app draws there
 * itself), an action bar when the theme has one, and the content frame (android.R.id.content). Its events go to the window's callback.
 */
public final class DecorView extends FrameLayout {
    private final Window mWindow;
    private Drawable mBackground;
    private LinearLayout mStack;
    private FrameLayout mContent;
    private android.widget.Toolbar mTitle;
    private View mActionBar;
    private boolean mCloseOnTouchOutside;
    private int mInsetTop, mInsetBottom, mInsetLeft, mInsetRight;
    /** Edge to edge: the status bar and home indicator areas the app was given as insets and draws under itself. */
    private int mEdgeTop, mEdgeBottom;
    private final Paint mBarPaint = new Paint();
    private static WindowInsetsController sFallback;
    private TypedValue mMinWidthMajor, mMinWidthMinor;
    private static TypedValue copy(TypedValue v) { if (v == null) return null; TypedValue c = new TypedValue(); c.type = v.type; c.data = v.data; return c; }

    public DecorView(Context c, Window w) {
        super(c);
        mWindow = w;
        setWillNotDraw(false);
        TypedArray a = c.obtainStyledAttributes(S.Window);
        mCloseOnTouchOutside = a.getBoolean(S.Window_windowCloseOnTouchOutside, false);
        mMinWidthMajor = copy(a.peekValue(S.Window_windowMinWidthMajor));
        mMinWidthMinor = copy(a.peekValue(S.Window_windowMinWidthMinor));
        a.recycle();
    }
    public static WindowInsetsController fallbackInsetsController() {
        if (sFallback == null) sFallback = new WindowInsetsController() {
            public void show(int t) {} public void hide(int t) {} public void setSystemBarsAppearance(int a, int m) {} public int getSystemBarsAppearance() { return 0; }
            public void setSystemBarsBehavior(int b) {} public int getSystemBarsBehavior() { return 0; }
        };
        return sFallback;
    }
    /** The window's controller, before the decor is attached too (Android's PendingInsetsController). */
    @Override public WindowInsetsController getWindowInsetsController() { WindowInsetsController c = super.getWindowInsetsController(); return c != null ? c : fallbackInsetsController(); }
    public void setCloseOnTouchOutside(boolean c) { mCloseOnTouchOutside = c; }
    public boolean closeOnTouchOutside() { return mCloseOnTouchOutside; }
    /** The window background; its padding (a Material dialog's InsetDrawable: the card inset from the window's edges) keeps the
     *  content inside it, as View.setBackground does on Android. */
    public void setWindowBackground(Drawable d) {
        mBackground = d;
        Rect p = new Rect();
        if (d != null) { d.setCallback(this); if (!d.getPadding(p)) p.setEmpty(); }
        setPadding(p.left, p.top, p.right, p.bottom);
        invalidate();
        requestLayout();
    }
    @Override protected boolean verifyDrawable(Drawable who) { return who == mBackground || super.verifyDrawable(who); }
    /* the window background is the decor's background on Android: apps animate its layers (Bitcoin Wallet's enter fade) */
    @Override public Drawable getBackground() { Drawable b = super.getBackground(); return b != null ? b : mBackground; }

    /** The views between the decor and the content: an action bar when asked for, then the content frame. */
    public FrameLayout makeContent(boolean actionBar, CharSequence title) {
        Context c = getContext();
        mStack = new LinearLayout(c);
        mStack.setOrientation(LinearLayout.VERTICAL);
        if (actionBar) {
            TypedValue v = new TypedValue();
            int h = c.getTheme().resolveAttribute(android.R.attr.actionBarSize, v, true) ? TypedValue.complexToDimensionPixelSize(v.data, getResources().getDisplayMetrics()) : (int) (56 * Native.density());
            int bg = c.getTheme().resolveAttribute(android.R.attr.colorPrimary, v, true) && v.type >= TypedValue.TYPE_FIRST_INT ? v.data : 0xFF212121;
            /* a Toolbar with the platform's id, as the Material action bar is (apps find it by android:id/action_bar) */
            mTitle = new android.widget.Toolbar(c);
            mTitle.setId(0x010201df);
            mTitle.setTitleTextColor(Color.luminance(bg) > 0.5f ? 0xDE000000 : 0xFFFFFFFF);
            mTitle.setTitle(title != null ? title : "");
            mTitle.setBackgroundColor(bg);
            mActionBar = mTitle;
            mStack.addView(mTitle, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, h));
        }
        mContent = new FrameLayout(c);
        mContent.setId(android.R.id.content);
        mStack.addView(mContent, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f));
        addView(mStack, new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        return mContent;
    }
    public void setTitle(CharSequence t) { if (mTitle != null) mTitle.setTitle(t); }
    public View actionBarView() { return mActionBar; }
    private View mTabStrip;
    /** The action bar's navigation tabs, in a row under it (null takes the row away). */
    public void setTabStrip(View strip) {
        if (mStack == null || strip == mTabStrip) return;
        if (mTabStrip != null) mStack.removeView(mTabStrip);
        mTabStrip = strip;
        if (strip != null) mStack.addView(strip, mActionBar != null ? mStack.indexOfChild(mActionBar) + 1 : 0, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, (int) (48 * Native.density())));
    }
    public void showActionBar(boolean show) { if (mActionBar != null) mActionBar.setVisibility(show ? VISIBLE : GONE); }

    /** A floating window (a dialog) is at least the theme's minimum width: a fraction of the screen's, or a size. */
    @Override protected void onMeasure(int ws, int hs) {
        if (mWindow.isFloating() && MeasureSpec.getMode(ws) == MeasureSpec.AT_MOST) {
            int sw = Native.screenWidth(), sh = Native.screenHeight();
            TypedValue tv = sw < sh ? mMinWidthMinor : mMinWidthMajor;
            int min = 0;
            if (tv != null) {
                if (tv.type == TypedValue.TYPE_DIMENSION) min = (int) tv.getDimension(getResources().getDisplayMetrics());
                else if (tv.type == TypedValue.TYPE_FRACTION) min = (int) tv.getFraction(sw, sw);
            }
            super.onMeasure(ws, hs);
            if (min > 0 && getMeasuredWidth() < min) super.onMeasure(MeasureSpec.makeMeasureSpec(Math.min(min, MeasureSpec.getSize(ws)), MeasureSpec.EXACTLY), hs);
            return;
        }
        super.onMeasure(ws, hs);
    }

    // ---- insets: the decor keeps its content off the bars unless the app lays out under them
    @Override public WindowInsets dispatchApplyWindowInsets(WindowInsets in) {
        boolean fits = mWindow.huskDecorFits() && !mWindow.isFloating();
        mInsetLeft = in.getSystemWindowInsetLeft(); mInsetTop = mWindow.huskFullscreen() ? 0 : in.getSystemWindowInsetTop();
        mInsetRight = in.getSystemWindowInsetRight(); mInsetBottom = in.getSystemWindowInsetBottom();
        if (fits) {
            if (mStack != null) {
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) mStack.getLayoutParams();
                lp.setMargins(mInsetLeft, mInsetTop, mInsetRight, mInsetBottom);
                mStack.setLayoutParams(lp);
            }
            mEdgeTop = mEdgeBottom = 0;
            WindowInsets rest = in.consumeSystemWindowInsets();
            super.dispatchApplyWindowInsets(rest);
            return rest;
        }
        if (mStack != null) { FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) mStack.getLayoutParams(); lp.setMargins(0, 0, 0, 0); mStack.setLayoutParams(lp); }
        mEdgeTop = mWindow.isFloating() ? 0 : mInsetTop;
        mEdgeBottom = mWindow.isFloating() || in.isVisible(WindowInsets.Type.ime()) ? 0 : mInsetBottom;
        mInsetTop = mInsetBottom = mInsetLeft = mInsetRight = 0;
        return super.dispatchApplyWindowInsets(in);
    }
    @Override public void draw(Canvas c) {
        if (mBackground != null) {
            mBackground.setBounds(0, 0, getWidth(), getHeight());
            mBackground.draw(c);
        }
        boolean screen = !mWindow.isFloating() && mWindow.getAttributes().type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
        if (screen) husk.ViewRoot.huskBandsBefore(mEdgeTop, mEdgeBottom);
        super.draw(c);
        boolean untouched = screen && husk.ViewRoot.huskBandsAfter(mEdgeTop, mEdgeBottom);
        // the bars' own colours over the areas the content was kept out of
        if (mInsetTop > 0 && !mWindow.isFloating()) { mBarPaint.setColor(mWindow.getStatusBarColor()); c.drawRect(0, 0, getWidth(), mInsetTop, mBarPaint); }
        if (mInsetBottom > 0 && !mWindow.isFloating()) { mBarPaint.setColor(mWindow.getNavigationBarColor()); c.drawRect(0, getHeight() - mInsetBottom, getWidth(), getHeight(), mBarPaint); }
        if (!untouched && (mEdgeTop > 0 || mEdgeBottom > 0) && mWindow.getAttributes().type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) runBarsToEdges(c);
        // ...which on an iPhone are not bars: the screen's own window asks ViewRoot to run the content's edges into them instead,
        // whatever the bars' colour (a transparent one shows the window below this one there, an activity the app left behind)
        if (!mWindow.isFloating() && mWindow.getAttributes().type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
            husk.ViewRoot.huskExtendEdges(mInsetTop, mInsetBottom);
            // an app that lays out under the bars (Compose's edge to edge) but keeps its content clear of them leaves those bands
            // to the window's background: they too take the content's edges, when nothing but that background is in them
            husk.ViewRoot.huskExtendIfBlank(mEdgeTop, mEdgeBottom);
        }
    }

    /* An app that keeps its content off the bars (fitsSystemWindows, or padding from the insets) leaves the areas under the
       clock and the home indicator to whatever is behind it: on an Android phone those are the bars, on an iPhone they are the
       app's own screen. As an iPhone app's top bar and tab bar do, the views that sit against those areas -- a toolbar at the
       top, a bottom navigation bar at the bottom -- run their own background on to the screen's edge. Only when nothing but
       the containers they sit in draws there, so an app that draws under the bars itself keeps what it draws. */
    private final int[] mHit = new int[4];
    private void runBarsToEdges(Canvas c) {
        int w = getWidth(), h = getHeight();
        if (mEdgeTop > 0) runBar(c, w, mEdgeTop + 1, mEdgeTop / 2, mEdgeTop, true, 0, mEdgeTop);
        if (mEdgeBottom > 0) runBar(c, w, h - mEdgeBottom - 1, h - mEdgeBottom / 2, h - mEdgeBottom, false, h - mEdgeBottom, h);
    }
    private void runBar(Canvas c, int w, int py, int bandY, int edge, boolean top, int y0, int y1) {
        View bar = find(this, 0, 0, w / 2, py, edge, top, w);
        if (bar == null) return;
        View inBand = deepest(this, 0, 0, w / 2, bandY);
        for (View v = bar; ; v = (View) v.getParent()) { if (v == inBand) break; if (v == this || !(v.getParent() instanceof View)) return; }
        Drawable bg = bar.getBackground();
        int[] r = mHit;
        Rect old = new Rect(bg.getBounds());
        c.save();
        c.clipRect(0, y0, w, y1);
        // the background drawn as it is on the bar, stretched by the band: a colour stays a colour, a gradient runs on
        bg.setBounds(r[0], top ? y0 : r[1], r[2], top ? r[3] : y1);
        bg.draw(c);
        bg.setBounds(old);
        c.restore();
    }
    private static boolean opaqueBg(View v) {
        Drawable d = v.getBackground();
        if (d == null || d.getAlpha() == 0 || v.getAlpha() < 1f) return false;
        if (d instanceof android.graphics.drawable.ColorDrawable) return android.graphics.Color.alpha(((android.graphics.drawable.ColorDrawable) d).getColor()) == 255;
        return d.getOpacity() != PixelFormat.TRANSPARENT;
    }
    /** The topmost full-width view with an opaque background whose top (or bottom) edge is at the band's, under (px, py). */
    private View find(ViewGroup g, int ox, int oy, int px, int py, int edge, boolean top, int w) {
        for (int i = g.getChildCount() - 1; i >= 0; i--) {
            View ch = g.getChildAt(i);
            if (ch.getVisibility() != View.VISIBLE) continue;
            int cx = ox + ch.getLeft() - g.getScrollX() + (int) ch.getTranslationX(), cy = oy + ch.getTop() - g.getScrollY() + (int) ch.getTranslationY();
            int cw = ch.getWidth(), chh = ch.getHeight();
            if (px < cx || px >= cx + cw || py < cy || py >= cy + chh) continue;
            if (ch instanceof ViewGroup) { View f = find((ViewGroup) ch, cx, cy, px, py, edge, top, w); if (f != null) return f; }
            if (cw * 10 >= w * 9 && Math.abs((top ? cy : cy + chh) - edge) <= 2 && opaqueBg(ch)) {
                mHit[0] = cx; mHit[1] = cy; mHit[2] = cx + cw; mHit[3] = cy + chh;
                return ch;
            }
            return null;    /* the topmost view under the point that is not the bar covers it */
        }
        return null;
    }
    /** The topmost view under (px, py). */
    private static View deepest(ViewGroup g, int ox, int oy, int px, int py) {
        for (int i = g.getChildCount() - 1; i >= 0; i--) {
            View ch = g.getChildAt(i);
            if (ch.getVisibility() != View.VISIBLE) continue;
            int cx = ox + ch.getLeft() - g.getScrollX() + (int) ch.getTranslationX(), cy = oy + ch.getTop() - g.getScrollY() + (int) ch.getTranslationY();
            if (px < cx || px >= cx + ch.getWidth() || py < cy || py >= cy + ch.getHeight()) continue;
            return ch instanceof ViewGroup ? deepest((ViewGroup) ch, cx, cy, px, py) : ch;
        }
        return g;
    }

    // ---- events
    @Override public boolean dispatchTouchEvent(MotionEvent e) {
        Window.Callback cb = mWindow.getCallback();
        if (e.getActionMasked() == MotionEvent.ACTION_OUTSIDE || (mCloseOnTouchOutside && e.getActionMasked() == MotionEvent.ACTION_DOWN && outside(e))) {
            if (cb != null && !mWindow.isDestroyed()) cb.dispatchTouchEvent(e);
            return true;
        }
        return cb != null && !mWindow.isDestroyed() ? cb.dispatchTouchEvent(e) : super.dispatchTouchEvent(e);
    }
    private boolean outside(MotionEvent e) { return e.getX() < 0 || e.getY() < 0 || e.getX() >= getWidth() || e.getY() >= getHeight(); }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        Window.Callback cb = mWindow.getCallback();
        return cb != null && !mWindow.isDestroyed() ? cb.dispatchKeyEvent(e) : super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        Window.Callback cb = mWindow.getCallback();
        return cb != null && !mWindow.isDestroyed() ? cb.dispatchGenericMotionEvent(e) : super.dispatchGenericMotionEvent(e);
    }
    public boolean superDispatchTouchEvent(MotionEvent e) { return super.dispatchTouchEvent(e); }
    public boolean superDispatchKeyEvent(KeyEvent e) {
        if (super.dispatchKeyEvent(e)) return true;
        return false;
    }
    public boolean superDispatchGenericMotionEvent(MotionEvent e) { return super.dispatchGenericMotionEvent(e); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); Window.Callback cb = mWindow.getCallback(); if (cb != null && !mWindow.isDestroyed()) cb.onWindowFocusChanged(f); }
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); Window.Callback cb = mWindow.getCallback(); if (cb != null && !mWindow.isDestroyed()) cb.onAttachedToWindow(); }
    @Override protected void onDetachedFromWindow() { super.onDetachedFromWindow(); Window.Callback cb = mWindow.getCallback(); if (cb != null) cb.onDetachedFromWindow(); }
    public Window getWindowHusk() { return mWindow; }
}
