# Habit Tracker

A simple web app to track three habits:

- **Exercise**: log the type, minutes, how hard it was, and notes.
- **Meditation**: use the built-in timer (a soft bell rings at the end) or add a session by hand.
- **Fasting**: start a live fasting timer with a goal (16:8, 18:6, 24 h and more), or add a past fast.

It also shows daily goals, streaks, a 7-day view and an 18-week history for each habit.

Your data is saved in your browser. You can **back it up to Google Drive** and load it
again on another phone or computer.

The app is plain HTML, CSS and JavaScript. There is no build step and no server code.

## Run it

The app must be opened from a web address (`http://` or `https://`), not by
double-clicking the file. Google sign-in does not work from `file://`.

On your computer:

```sh
cd habit-tracker
python3 -m http.server 8000
```

Then open <http://localhost:8000>.

To use it on your phone, put the `habit-tracker` folder on any static web host
(for example GitHub Pages, Netlify or Cloudflare Pages). On the phone, use
"Add to Home Screen" so it opens like an app.

## Turn on Google Drive backup (one time)

You need a free Google OAuth **Client ID**. This lets the app ask for permission
to save its backup file in your Drive.

1. Open [Google Cloud Console](https://console.cloud.google.com/) and create a project.
2. Go to **APIs & Services → Library** and turn on **Google Drive API**.
3. Go to **APIs & Services → OAuth consent screen**.
   - Pick **External**.
   - Fill in the app name and your email.
   - Add your own Google account as a **test user**.
4. Go to **APIs & Services → Credentials → Create credentials → OAuth client ID**.
   - Application type: **Web application**.
   - Under **Authorized JavaScript origins**, add the address where you open the app,
     for example `http://localhost:8000` or `https://yourname.github.io`.
     (No path and no slash at the end.)
5. Copy the **Client ID** (it ends with `.apps.googleusercontent.com`).
6. In the app, open **Settings**, paste the Client ID, tap **Save Client ID**,
   then tap **Connect Google Drive**.

### How the backup works

- The backup is one file named `habit-tracker-backup.json` in your "My Drive".
- The app uses the `drive.file` permission. It can only see files it created.
  It cannot read your other Drive files.
- With **Back up automatically** on, the app saves to Drive a few seconds after each change.
- Google's sign-in lasts about one hour. After that, the button at the top shows
  **Tap to back up**. Tap it to sign in again and save.
- Google Drive keeps older versions of the file. To see them, right-click the
  file in Drive and choose **Manage versions**.
- On a new device, connect Drive and the app will offer to load your backup.
  You can also use **Settings → Restore from Drive** at any time.

You can also use **Download backup (.json)** and **Load backup file** to save a
copy on your device without Google Drive.

## Files

| File            | What it does                              |
| --------------- | ----------------------------------------- |
| `index.html`    | Page layout and bottom tabs               |
| `style.css`     | Styles (light and dark mode)              |
| `app.js`        | All app logic, timers and Google Drive    |
| `manifest.json` | Lets you add the app to your home screen  |
| `icon.svg`      | App icon                                  |
